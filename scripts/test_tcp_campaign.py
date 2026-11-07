#!/usr/bin/env python3
"""Verify bulk TCP interoperability and an impaired guest-to-guest Ethernet link."""

import argparse
from concurrent.futures import ThreadPoolExecutor
from functools import lru_cache
from hashlib import sha256
import json
from pathlib import Path
import re
import socket
import struct
import tempfile

from ethernet_link import link
from evidence_format import file_hash
from test_smoke import PROMPT, require
from vm import BOOT_IMAGE, boot_profile, command, running


PORT = 9090
SYN, ACK, FIN, RST = 2, 16, 1, 4
BULK_SESSIONS = 800
BULK_BYTES = 192 * 1024
IMPAIRED_SESSIONS = 200
IMPAIRED_BYTES = 4096


@lru_cache(maxsize=256)
def payload(seed, size):
    return bytes(((seed * 31 + i * 17) ^ (i >> 8)) & 255 for i in range(size))


def tcp(frame):
    if len(frame) < 54 or frame[12:14] != b"\x08\x00" or frame[23] != 6:
        return None
    start = 14 + (frame[14] & 15) * 4
    end = 14 + int.from_bytes(frame[16:18], "big")
    if start + 20 > end or end > len(frame):
        raise RuntimeError("truncated TCP packet")
    source, destination, seq = struct.unpack_from("!HHI", frame, start)
    offset = start + (frame[start + 12] >> 4) * 4
    if offset > end:
        raise RuntimeError("truncated TCP header")
    return source, destination, seq, frame[start + 13], frame[offset:end]


class Impairment:
    def __init__(self, skip_loss=0):
        self.skip_loss = skip_loss
        self.server_segments = {}
        self.profiles = {}
        self.applied = {}
        self.pending = {}

    def transform(self, sender, packet):
        segment = tcp(packet[4:])
        if segment is None:
            return (packet,)
        source, destination, sequence, flags, data = segment
        peer = source if sender == 1 else destination
        if sender == 1 and destination == PORT and flags & (SYN | ACK) == SYN:
            self.profiles.setdefault(peer, ("loss", "duplicate", "reorder")[len(self.profiles) % 3])
        profile = self.profiles.get(peer)
        if not data or profile is None or peer in self.applied:
            return (packet,)
        if profile == "loss" and sender == 0:
            seen = self.server_segments.get(peer, 0)
            self.server_segments[peer] = seen + 1
            if seen < self.skip_loss:
                return (packet,)
            self.applied[peer] = (profile, sequence)
            return ()
        if profile == "duplicate" and sender == 1:
            self.applied[peer] = (profile, sequence)
            return (packet, packet)
        if profile == "reorder" and sender == 1:
            older = self.pending.get(peer)
            if older is None:
                self.pending[peer] = (sequence, packet)
            elif sequence != older[0]:
                del self.pending[peer]
                self.applied[peer] = (profile, older[0])
                return (packet, older[1])
        return (packet,)


def frames(capture):
    with capture.open("rb") as stream:
        header = stream.read(24)
        endian = {b"\xd4\xc3\xb2\xa1": "<", b"\xa1\xb2\xc3\xd4": ">"}.get(header[:4])
        if endian is None or struct.unpack_from(endian + "I", header, 20)[0] != 1:
            raise RuntimeError("expected Ethernet pcap")
        while raw := stream.read(16):
            _, _, captured, original = struct.unpack(endian + "4I", raw)
            frame = stream.read(captured)
            if captured != original or not 14 <= captured <= 1518 or len(frame) != captured:
                raise RuntimeError("truncated Ethernet frame")
            yield frame


class WireStream:
    def __init__(self, size):
        self.body = bytearray(size)
        self.present = bytearray(size)
        self.remaining = size
        self.finished = False

    def add(self, offset, data):
        end = offset + len(data)
        if end > len(self.body):
            raise RuntimeError("TCP payload exceeded the declared transfer length")
        previous = self.present[offset:end]
        absent = previous.count(0)
        if absent != len(data):
            observed = self.body[offset:end]
            if any(prior and old != new for prior, old, new in zip(previous, observed, data)):
                raise RuntimeError("conflicting wire retransmission")
        self.body[offset:end] = data
        self.present[offset:end] = b"\x01" * len(data)
        self.remaining -= absent
        return absent != len(data)


def verify_wire(capture, sessions, size, impairment=None):
    active = {}
    complete = {}
    retransmitted = set()
    for frame in frames(capture):
        segment = tcp(frame)
        if segment is None:
            continue
        sport, dport, sequence, flags, data = segment
        if PORT not in (sport, dport):
            continue
        to_guest = dport == PORT
        port = sport if to_guest else dport
        direction = 0 if to_guest else 1
        if to_guest and flags & (SYN | ACK) == SYN:
            active.setdefault(port, {"starts": [(sequence + 1) & 0xffffffff, None],
                                     "streams": [WireStream(size + 8), WireStream(size)]})
        row = active.get(port)
        if row is None:
            continue
        if not to_guest and flags & (SYN | ACK) == SYN | ACK:
            row["starts"][1] = (sequence + 1) & 0xffffffff
        stream = row["streams"][direction]
        if data or flags & FIN:
            start = row["starts"][direction]
            if start is None:
                raise RuntimeError("observed data without a handshake")
            offset = (sequence - start) & 0xffffffff
            if data and stream.add(offset, data) and not to_guest:
                retransmitted.add(port)
            if flags & FIN:
                if offset + len(data) != len(stream.body):
                    raise RuntimeError(f"FIN at wrong sequence for client {port}")
                stream.finished = True
        if not all(item.finished and not item.remaining for item in row["streams"]):
            if flags & RST:
                raise RuntimeError(f"wire stream reset before both FINs for client {port}")
            continue
        sent, echoed = (item.body for item in row["streams"])
        identity, length = struct.unpack_from("<II", sent)
        if identity in complete or identity >= sessions or length != size:
            raise RuntimeError(f"unexpected transfer identity {identity}")
        expected = payload(identity & 255, size)
        if sent[8:] != expected or echoed != expected:
            raise RuntimeError(f"wire payload differs for transfer {identity}")
        complete[identity] = sha256(echoed).digest()
        del active[port]
    if len(complete) != sessions or active:
        raise RuntimeError(f"expected {sessions} closed wire streams, found {len(complete)}")
    if impairment is not None:
        lost = {port for port, (mode, _) in impairment.applied.items() if mode == "loss"}
        if len(impairment.applied) != sessions or not lost <= retransmitted:
            raise RuntimeError("missing an impairment or its retransmission")
    return sha256(b"".join(complete[i] for i in range(sessions))).hexdigest()


def start_server(guest, sessions):
    guest.expect(PROMPT)
    guest.send(f"/bin/tcp-campaign serve {sessions}\r".encode())
    guest.expect(f"tcp-campaign: ready for {sessions} sessions")


def server_completed(guest, sessions, size):
    summary = f"[PASS] TCP server sessions={sessions} bytes={sessions * size * 2}".encode()
    reply = guest.expect(PROMPT, timeout=35)
    require(summary, reply)
    count = re.search(rb"fast_retransmits=(\d+)", reply)
    return int(count[1]) if count else 0


def reference_client(port, identity, size):
    data = payload(identity & 255, size)
    with socket.create_connection(("127.0.0.1", port), timeout=25) as client:
        client.sendall(struct.pack("<II", identity, size) + data)
        client.shutdown(socket.SHUT_WR)
        actual = bytearray()
        while chunk := client.recv(65536):
            actual.extend(chunk)
            if len(actual) > size:
                raise RuntimeError(f"extra TCP bytes for session {identity}")
        if actual != data:
            raise RuntimeError(f"wrong TCP response for session {identity}, bytes={len(actual)}")


def reference_run(sessions, size, logs):
    with socket.socket() as reservation:
        reservation.bind(("127.0.0.1", 0))
        port = reservation.getsockname()[1]
    capture = logs / "host-ethernet.pcap"
    cmd = command(BOOT_IMAGE, cpus=4, host_port=port, capture=capture)
    with running(cmd, logs / "host-server.log") as server:
        start_server(server, sessions)
        with ThreadPoolExecutor(max_workers=12) as pool:
            for identity, _ in enumerate(pool.map(lambda i: reference_client(port, i, size), range(sessions)), 1):
                if identity % 100 == 0:
                    print(f"Verified {identity} host-reference TCP sessions", flush=True)
        server_completed(server, sessions, size)
    return {"sessions": sessions, "bytes": sessions * size * 2, "pcap_sha256": file_hash(capture),
            "wire_payload_sha256": verify_wire(capture, sessions, size)}


def impaired_run(sessions, logs, size=IMPAIRED_BYTES, *, skip_loss=0):
    capture = logs / "impaired-ethernet.pcap"
    faults = Impairment(skip_loss)
    with tempfile.TemporaryDirectory(dir="build") as directory, link(faults.transform) as (_, (p1, p2)):
        follower = boot_profile(BOOT_IMAGE, Path(directory), "ip=10.0.2.16")
        server_cmd = command(BOOT_IMAGE, cpus=2, ethernet=f"connect=127.0.0.1:{p1}", capture=capture)
        client_cmd = command(follower, cpus=2, ethernet=f"connect=127.0.0.1:{p2}", mac="52:54:00:12:34:57")
        with running(server_cmd, logs / "impaired-server.log") as server:
            start_server(server, sessions)
            with running(client_cmd, logs / "impaired-client.log") as guest:
                guest.expect(PROMPT)
                guest.send(f"/bin/tcp-campaign send {sessions} {size}\r".encode())
                summary = f"[PASS] TCP client sessions={sessions} bytes={sessions * size * 2}"
                require(summary.encode(), guest.expect(PROMPT, timeout=240))
            fast = server_completed(server, sessions, size)
    digest = verify_wire(capture, sessions, size, faults)
    return {"sessions": sessions, "bytes": sessions * size * 2, "pcap_sha256": file_hash(capture),
            "wire_payload_sha256": digest, "impairments": faults.applied, "fast_retransmits": fast}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bulk", type=int, default=BULK_SESSIONS)
    parser.add_argument("--size", type=int, default=BULK_BYTES)
    parser.add_argument("--impaired", type=int, default=IMPAIRED_SESSIONS)
    args = parser.parse_args()
    if not 0 <= args.bulk <= BULK_SESSIONS or not 0 <= args.impaired <= IMPAIRED_SESSIONS or not 1 <= args.size <= BULK_BYTES:
        parser.error("invalid campaign size")
    logs = Path(tempfile.mkdtemp(prefix="tcp-campaign-", dir="build"))
    kernel = Path("build/stratum.elf")
    kernel_hash, boot_hash = file_hash(kernel), file_hash(BOOT_IMAGE)
    print(f"Writing TCP evidence to {logs}", flush=True)
    rows = {}
    if args.bulk:
        rows["host"] = reference_run(args.bulk, args.size, logs)
    if args.impaired:
        rows["impaired"] = impaired_run(args.impaired, logs)
    if (file_hash(kernel), file_hash(BOOT_IMAGE)) != (kernel_hash, boot_hash):
        raise RuntimeError("image changed during TCP campaign")
    result = {"kernel_sha256": kernel_hash, "boot_image_sha256": boot_hash, "results": rows}
    (logs / "results.json").write_text(json.dumps(result, indent=2) + "\n")
    print(f"[PASS] {sum(row['sessions'] for row in rows.values())} TCP sessions and "
          f"{sum(row['bytes'] for row in rows.values())} exact wire payload bytes")
