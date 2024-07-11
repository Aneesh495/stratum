#!/usr/bin/env python3
"""Check native ICMP echo and bad-checksum rejection at the Ethernet boundary."""

from hashlib import sha256
import json
from pathlib import Path
import socket
import struct

from evidence_format import file_hash
from test_net_wrap import GUEST_IP, GUEST_MAC, PEER_IP, PEER_MAC, arp_exchange, checksum, framed, recv_frame
from test_smoke import PROMPT
from test_tcp_campaign import frames
from vm import BOOT_IMAGE, command, running


CASES = 256
IDENTIFIER = 0x69A5


def payload(sequence):
    size = (sequence * 139) % 1473
    return bytes((sequence * 23 + i * 41) & 255 for i in range(size))


def request(sequence, *, corrupt=False):
    body = struct.pack("!BBHHH", 8, 0, 0, IDENTIFIER, sequence) + payload(sequence)
    body = body[:2] + struct.pack("!H", checksum(body) ^ corrupt) + body[4:]
    ip = struct.pack("!BBHHHBBH4s4s", 0x45, 0, 20 + len(body), sequence, 0x4000,
                     64, 1, 0, PEER_IP, GUEST_IP)
    ip = ip[:10] + struct.pack("!H", checksum(ip)) + ip[12:]
    return (GUEST_MAC + PEER_MAC + b"\x08\x00" + ip + body).ljust(60, b"\0")


def reply(frame):
    if frame[:14] != PEER_MAC + GUEST_MAC + b"\x08\x00" or frame[14] != 0x45:
        raise RuntimeError("wrong ICMP reply Ethernet or IPv4 header")
    ip = frame[14:34]
    if checksum(ip) or ip[9] != 1 or ip[12:] != GUEST_IP + PEER_IP:
        raise RuntimeError("wrong ICMP IPv4 checksum, addresses or protocol")
    end = 14 + int.from_bytes(ip[2:4], "big")
    if end > len(frame) or end < 42:
        raise RuntimeError("truncated ICMP echo")
    message = frame[34:end]
    kind, code, _, identity, sequence = struct.unpack_from("!BBHHH", message)
    if checksum(message) or kind or code or identity != IDENTIFIER or message[8:] != payload(sequence):
        raise RuntimeError("wrong ICMP reply identity, checksum or payload")
    return sequence, message[8:]


def verify_capture(capture):
    digest = sha256()
    observed, requests = [], []
    for packet in frames(capture):
        if packet[:14] == PEER_MAC + GUEST_MAC + b"\x08\x00":
            sequence, actual = reply(packet)
            observed.append(sequence)
            digest.update(actual)
        elif packet[:14] == GUEST_MAC + PEER_MAC + b"\x08\x00":
            sequence = int.from_bytes(packet[40:42], "big")
            if packet != request(sequence, corrupt=sequence >= CASES):
                raise RuntimeError("captured ICMP request disagrees with fixture")
            requests.append(sequence)
    if requests != [value for sequence in range(CASES) for value in (sequence + CASES, sequence)]:
        raise RuntimeError("missing or reordered ICMP input cases")
    if observed != list(range(CASES)):
        raise RuntimeError("missing, duplicate or invalid ICMP replies on the wire")
    return digest.hexdigest()


def main():
    logs = Path("build/icmp")
    logs.mkdir(parents=True, exist_ok=True)
    capture = logs / "ethernet.pcap"
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        listener.listen()
        listener.settimeout(20)
        port = listener.getsockname()[1]
        cmd = command(BOOT_IMAGE, cpus=2, ethernet=f"connect=127.0.0.1:{port}", capture=capture)
        with running(cmd, logs / "guest.log") as guest:
            peer, _ = listener.accept()
            with peer, peer.makefile("rb") as reader:
                peer.settimeout(10)
                guest.expect(PROMPT)
                arp_exchange(peer, reader)
                for sequence in range(CASES):
                    peer.sendall(framed(request(sequence + CASES, corrupt=True)) + framed(request(sequence)))
                    observed, _ = reply(recv_frame(reader))
                    if observed != sequence:
                        raise RuntimeError(f"expected ICMP sequence {sequence}, saw {observed}")
    report = {"valid_echoes": CASES, "rejected_checksums": CASES, "capture_sha256": file_hash(capture),
              "boot_sha256": file_hash(BOOT_IMAGE), "payload_sha256": verify_capture(capture)}
    (logs / "results.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"[PASS] {CASES} exact ICMP echoes with {CASES} bad checksums interleaved")


if __name__ == "__main__":
    main()
