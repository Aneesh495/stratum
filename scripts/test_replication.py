#!/usr/bin/env python3
"""Transfer one durable record between two Stratum guests on an isolated Ethernet link."""

from pathlib import Path
import socket
import struct
import tempfile

from test_smoke import PROMPT, require, shell
from vm import BOOT_IMAGE, boot_profile, command, fresh_disk, running


def check_capture(capture, request, response):
    data = capture.read_bytes()
    endian = {b"\xd4\xc3\xb2\xa1": "<", b"\xa1\xb2\xc3\xd4": ">"}.get(data[:4])
    if endian is None or struct.unpack_from(endian + "I", data, 20)[0] != 1:
        raise RuntimeError("expected Ethernet pcap")
    records = {}
    arp_seen = False
    offset = 24
    while offset < len(data):
        _, _, captured, original = struct.unpack_from(endian + "4I", data, offset)
        offset += 16
        frame = data[offset:offset + captured]
        offset += captured
        if len(frame) != captured or captured != original:
            raise RuntimeError("truncated Ethernet capture")
        if frame[12:14] == b"\x08\x06":
            arp_seen = True
        if frame[12:14] != b"\x08\x00" or len(frame) < 54 or frame[23] != 6:
            continue
        ip_header = (frame[14] & 15) * 4
        end = 14 + int.from_bytes(frame[16:18], "big")
        segment = frame[14 + ip_header:end]
        src, dst = socket.inet_ntoa(frame[26:30]), socket.inet_ntoa(frame[30:34])
        sport, dport = struct.unpack_from("!HH", segment)
        payload = segment[(segment[12] >> 4) * 4:]
        if 9090 in (sport, dport) and payload:
            records.setdefault((src, dst), {})[segment[4:8]] = payload
    if not arp_seen:
        raise RuntimeError("capture lacks ARP discovery")
    for flow, expected in ((("10.0.2.16", "10.0.2.15"), request),
                           (("10.0.2.15", "10.0.2.16"), response)):
        chunks = records.get(flow, {})
        stream = b"".join(chunks[seq] for seq in sorted(chunks))
        if stream != expected:
            raise RuntimeError(f"unexpected Ethernet flow {flow}: {stream!r}")


def main():
    log = Path("build/replication")
    log.mkdir(parents=True, exist_ok=True)
    capture = log / "ethernet.pcap"
    with socket.socket() as host:
        host.bind(("127.0.0.1", 0))
        port = host.getsockname()[1]
    with fresh_disk() as primary_disk, fresh_disk() as follower_disk, tempfile.TemporaryDirectory(dir="build") as directory:
        follower_boot = boot_profile(BOOT_IMAGE, Path(directory), "ip=10.0.2.16")
        primary = command(BOOT_IMAGE, primary_disk, cpus=2, ethernet=f"listen=127.0.0.1:{port}",
                          capture=capture)
        follower = command(follower_boot, follower_disk, cpus=2, ethernet=f"connect=127.0.0.1:{port}",
                           mac="52:54:00:12:34:57")
        with running(primary, log / "primary.log") as source:
            source.expect(PROMPT)
            require(b"Ledger record durable.", shell(source, "ledgerput peer-1 via-ethernet"))
            source.send(b"ledgerd\r")
            source.expect("ledgerd listening on port 9090.")
            with running(follower, log / "follower.log") as guest:
                guest.expect(PROMPT)
                require(b"Ledger record durable.", shell(guest, "ledgerpull 10.0.2.15 peer-1"))
                if shell(guest, "cat /ledger/peer-1") != b"via-ethernet":
                    raise RuntimeError("follower stored incorrect bytes")
                guest.interrupt()

        check_capture(capture, b"GET peer-1\n", b"OK via-ethernet\n")
        with running(command(BOOT_IMAGE, follower_disk, cpus=2), log / "follower-reboot.log") as guest:
            guest.expect(PROMPT)
            if shell(guest, "cat /ledger/peer-1") != b"via-ethernet":
                raise RuntimeError("follower record did not survive reboot")

    print("[PASS] Two guests transferred and persisted a record over captured Ethernet frames")


if __name__ == "__main__":
    main()
