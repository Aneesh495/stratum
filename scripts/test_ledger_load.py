#!/usr/bin/env python3
"""Verify native ledger clients, automatic replication, and guest crash recovery."""

import argparse
from contextlib import ExitStack, contextmanager
from hashlib import sha256
import json
from pathlib import Path
import re
import select
import socket
import struct
import subprocess
import tempfile
from threading import Event, Thread

from evidence_format import file_hash
from test_smoke import PROMPT, require
from vm import BOOT_IMAGE, boot_profile, command, fresh_disk, running, tool


class Relay:
    """Forward framed Ethernet and permit either VM to reconnect after reboot."""

    def __init__(self, listeners):
        self.listeners = listeners
        self.stop = Event()
        self.error = None
        self.accepted = [0, 0]

    def run(self):
        peers = [None, None]
        pending = [bytearray(), bytearray()]

        def close_peer(index):
            if peers[index] is not None:
                peers[index].close()
                peers[index] = None
            pending[index].clear()

        try:
            while not self.stop.is_set():
                ready, _, _ = select.select([*self.listeners, *(p for p in peers if p)], [], [], 0.1)
                for endpoint in ready:
                    if endpoint in self.listeners:
                        index = self.listeners.index(endpoint)
                        close_peer(index)
                        peers[index], _ = endpoint.accept()
                        peers[index].settimeout(2)
                        self.accepted[index] += 1
                        continue
                    if endpoint not in peers:
                        continue
                    index = peers.index(endpoint)
                    try:
                        data = endpoint.recv(65536)
                    except ConnectionError:
                        data = b""
                    if not data:
                        close_peer(index)
                        continue
                    buffer = pending[index]
                    buffer.extend(data)
                    while len(buffer) >= 4:
                        size, = struct.unpack_from("!I", buffer)
                        if not 14 <= size <= 1518:
                            raise RuntimeError(f"invalid Ethernet frame length {size}")
                        if len(buffer) < size + 4:
                            break
                        packet = bytes(buffer[:size + 4])
                        del buffer[:size + 4]
                        target = 1 - index
                        if peers[target] is not None:
                            try:
                                peers[target].sendall(packet)
                            except ConnectionError:
                                close_peer(target)
        except Exception as error:
            self.error = error
        finally:
            for index in range(2):
                close_peer(index)


@contextmanager
def link():
    with ExitStack() as stack:
        listeners = [stack.enter_context(socket.socket()) for _ in range(2)]
        for listener in listeners:
            listener.bind(("127.0.0.1", 0))
            listener.listen(1)
        relay = Relay(listeners)
        worker = Thread(target=relay.run, daemon=True)
        worker.start()
        try:
            yield relay, [listener.getsockname()[1] for listener in listeners]
        finally:
            relay.stop.set()
            worker.join(timeout=5)
            if worker.is_alive() or relay.error:
                raise RuntimeError(f"Ethernet relay failed: {relay.error}")


class RecordImage:
    """Read the append log from a quiescent raw StrataFS image."""

    BLOCK = 4096

    def __init__(self, disk):
        self.disk = disk
        superblock = self.block(1)
        if struct.unpack_from("<QII", superblock) != (0x5354524154414653, 1, self.BLOCK):
            raise RuntimeError("wrong StrataFS image")
        self.inode_table, = struct.unpack_from("<I", superblock, 60)

    def block(self, block_number):
        self.disk.seek(block_number * self.BLOCK)
        data = self.disk.read(self.BLOCK)
        if len(data) != self.BLOCK:
            raise RuntimeError("short StrataFS block")
        return data

    def inode_bytes(self, number):
        self.disk.seek(self.inode_table * self.BLOCK + number * 128)
        node = self.disk.read(128)
        size, = struct.unpack_from("<Q", node, 8)
        blocks = list(struct.unpack_from("<12I", node, 48))
        indirect, = struct.unpack_from("<I", node, 96)
        if indirect:
            blocks.extend(struct.unpack("<1024I", self.block(indirect)))
        needed = (size + self.BLOCK - 1) // self.BLOCK
        if needed > len(blocks) or not all(blocks[:needed]):
            raise RuntimeError("unexpected ledger block layout")
        return b"".join(self.block(block) for block in blocks[:needed])[:size]

    def entry(self, inode, name):
        entries = self.inode_bytes(inode)
        for number, width, length, _, entry in struct.iter_unpack("<IHBB248s", entries):
            if number and width == 256 and entry[:length] == name:
                return number
        raise RuntimeError(f"missing StrataFS directory entry {name!r}")

    def ledger(self):
        return self.inode_bytes(self.entry(self.entry(1, b"ledger"), b".records-v1"))


def expected_record(key):
    value = bytes(97 + (((key * 13) ^ (i * 29 + key // 100)) % 26) for i in range(64))
    return f"entry{key:04d}".encode(), value


def checksum(data):
    value = 2166136261
    for byte in data:
        value = ((value ^ byte) * 16777619) & 0xFFFFFFFF
    return value


def check_records(path, keys, *, final=False):
    with path.open("rb") as disk:
        data = RecordImage(disk).ledger()
    if len(data) % 160:
        raise RuntimeError("ledger ended mid-record")
    records = {}
    for digest, identity, value, reserved in struct.iter_unpack("<I25s129s2s", data):
        identity, value = identity.rstrip(b"\0"), value.rstrip(b"\0")
        raw = struct.pack("<25s129s2s", identity, value, reserved)
        if reserved != b"\0\0" or digest != checksum(raw) or identity in records:
            raise RuntimeError(f"bad or duplicate ledger record {identity!r}")
        records[identity] = value
    for key in keys:
        identity, value = expected_record(key)
        if records.get(identity) != value:
            raise RuntimeError(f"acknowledged record missing or wrong: {identity!r}")
    if final and len(records) != len(keys):
        raise RuntimeError(f"expected {len(keys)} distinct records, observed {len(records)}")
    return sha256(b"".join(key + records[key] for key in sorted(records))).hexdigest()


def start_primary(guest, *, first=False):
    raw = guest.expect(PROMPT)
    if not first:
        require(b"Mounted StrataFS on '/'", raw)
    guest.send(b"ledgerd 10.0.2.16\r")
    guest.expect("ledgerd listening on port 9090.")


def run(keys, crashes, logs):
    kernel = Path("build/stratum.elf")
    kernel_hash, boot_hash = file_hash(kernel), file_hash(BOOT_IMAGE)
    checker = logs / "strata_check"
    subprocess.run([tool("clang"), "-O2", "tools/strata_check.c", "-o", str(checker)], check=True, timeout=30)
    markers = []
    durable = set()
    capture = logs / "ethernet.pcap"
    with fresh_disk() as primary_disk, fresh_disk() as follower_disk, tempfile.TemporaryDirectory(dir="build") as tmp:
        follower_boot = boot_profile(BOOT_IMAGE, Path(tmp), "ip=10.0.2.16")
        with link() as (relay, (primary_port, follower_port)), ExitStack() as source:
            primary_cmd = command(BOOT_IMAGE, primary_disk, cpus=4, ethernet=f"connect=127.0.0.1:{primary_port}")
            follower_cmd = command(follower_boot, follower_disk, cpus=4,
                                   ethernet=f"connect=127.0.0.1:{follower_port}", mac="52:54:00:12:34:57",
                                   capture=capture)
            primary = source.enter_context(running(primary_cmd, logs / "primary-00.log"))
            start_primary(primary, first=True)
            with running(follower_cmd, logs / "follower.log") as follower:
                follower.expect(PROMPT)
                follower.send(f"/bin/ledger-campaign {keys} | ledgerd\r".encode())
                follower.expect("[LEDGER] clients=32 live sessions", timeout=70)
                for i in range(crashes):
                    target = (i + 1) * keys * 10 // ((crashes + 1) * 1000) * 1000
                    serial = follower.expect(f"[LEDGER] requests={target}\r\n", timeout=240)
                    durable.update(int(n) for n in re.findall(rb"\[RECORD\] entry(\d{4}) durable\r\n", serial))
                    if not durable:
                        raise RuntimeError("interruption without durable acknowledgments")
                    primary.interrupt()
                    source.close()
                    digest = check_records(primary_disk, durable)
                    markers.append({"requests": target, "acknowledged_records": len(durable), "disk_digest": digest})
                    primary = source.enter_context(running(primary_cmd, logs / f"primary-{i+1:02d}.log"))
                    start_primary(primary)
                    print(f"Recovered from interruption {i+1}/{crashes} after {target} requests", flush=True)

                summary = f"[PASS] ledger requests={keys * 10} records={keys} clients=32 attempts="
                follower.expect(summary, timeout=600)
                follower.expect(PROMPT, timeout=25)
                primary.send(b"q")
                primary.expect(PROMPT, timeout=25)
            source.close()
            if relay.accepted != [crashes + 1, 1]:
                raise RuntimeError(f"wrong number of guest links {relay.accepted}")

        first = check_records(primary_disk, range(keys), final=True)
        second = check_records(follower_disk, range(keys), final=True)
        if first != second:
            raise RuntimeError("primary and replica hold different payloads")
        for disk, name in ((primary_disk, "primary"), (follower_disk, "follower")):
            with (logs / f"{name}-check.log").open("w") as raw:
                subprocess.run([str(checker), str(disk)], stdout=raw, stderr=subprocess.STDOUT, check=True, timeout=30)
        # Reboot each disk and use the guest's independent protocol client on the other copy.
        with link() as (_, (primary_port, follower_port)):
            primary_cmd = command(BOOT_IMAGE, primary_disk, cpus=4, ethernet=f"connect=127.0.0.1:{primary_port}")
            follower_cmd = command(follower_boot, follower_disk, cpus=4,
                                   ethernet=f"connect=127.0.0.1:{follower_port}", mac="52:54:00:12:34:57")
            with running(primary_cmd, logs / "primary-final-boot.log") as primary:
                start_primary(primary)
                with running(follower_cmd, logs / "follower-final-boot.log") as follower:
                    follower.expect(PROMPT)
                    follower.send(f"ledgerpull 10.0.2.15 entry{keys-1:04d}\r".encode())
                    require(b"Ledger record durable.", follower.expect(PROMPT, timeout=10))

    if (file_hash(kernel), file_hash(BOOT_IMAGE)) != (kernel_hash, boot_hash):
        raise RuntimeError("image changed during the ledger campaign")
    report = {
        "clients": 32, "logical_requests": keys * 10, "durable_records": keys, "interruptions": markers,
        "record_digest": first, "pcap_sha256": file_hash(capture),
        "kernel_sha256": kernel_hash, "boot_image_sha256": boot_hash,
    }
    (logs / "results.json").write_text(json.dumps(report, indent=2) + "\n")
    print(f"[PASS] 32 clients, {keys * 10} verified requests, {keys} replicated records, {crashes} recoveries")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--records", type=int, default=10000)
    parser.add_argument("--interruptions", type=int, default=10)
    args = parser.parse_args()
    if not 32 <= args.records <= 10000 or not 0 <= args.interruptions <= 10:
        parser.error("expected 32 to 10000 records and 0 to 10 interruptions")
    if args.interruptions and args.records * 10 < (args.interruptions + 1) * 1000:
        parser.error("request volume must allow at least 1000 requests between interruptions")
    logs = Path(tempfile.mkdtemp(prefix="ledger-load-", dir="build"))
    print(f"Writing ledger evidence to {logs}", flush=True)
    run(args.records, args.interruptions, logs)
