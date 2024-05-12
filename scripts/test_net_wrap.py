#!/usr/bin/env python3
"""Verify UDP and both network rings through an independent Ethernet peer."""

import argparse
from hashlib import sha256
import json
from pathlib import Path
import re
import socket
import struct
import time

from evidence_format import file_hash
from test_smoke import PROMPT, require
from vm import BOOT_IMAGE, command, running


GUEST_MAC = bytes.fromhex("525400123456")
PEER_MAC = bytes.fromhex("020000000016")
GUEST_IP = socket.inet_aton("10.0.2.15")
PEER_IP = socket.inet_aton("10.0.2.16")
PORT = 40960
WINDOW = 8
REQUIRED = 150000


def checksum(data):
    if len(data) & 1:
        data += b"\0"
    total = sum(word for word, in struct.iter_unpack("!H", data))
    while total >> 16:
        total = (total & 0xFFFF) + (total >> 16)
    return 0xFFFF ^ total


def expected_payload(sequence):
    return sequence.to_bytes(4, "little") + bytes(
        ((sequence * 31 + offset * 17) ^ (sequence // 256)) & 255
        for offset in range(4, 48 + sequence % 117)
    )


def frame(payload, sequence):
    length = len(payload) + 8
    header = struct.pack("!HHHH", PORT, PORT, length, 0)
    check = checksum(PEER_IP + GUEST_IP + struct.pack("!BBH", 0, 17, length) + header + payload) or 0xFFFF
    udp = header[:6] + struct.pack("!H", check) + payload
    ip = struct.pack("!BBHHHBBH4s4s", 0x45, 0, 20 + len(udp), sequence & 0xFFFF, 0x4000,
                     64, 17, 0, PEER_IP, GUEST_IP)
    ip = ip[:10] + struct.pack("!H", checksum(ip)) + ip[12:]
    return GUEST_MAC + PEER_MAC + b"\x08\x00" + ip + udp


def recv_frame(reader):
    size, = struct.unpack("!I", reader.read(4))
    if not 14 <= size <= 1518:
        raise RuntimeError(f"malformed frame from QEMU: size={size}")
    data = reader.read(size)
    if len(data) != size:
        raise RuntimeError("incomplete frame from QEMU")
    return data


def framed(data):
    return struct.pack("!I", len(data)) + data


def arp_exchange(peer, reader):
    request = b"\xff" * 6 + PEER_MAC + b"\x08\x06"
    request += struct.pack("!HHBBH6s4s6s4s", 1, 0x0800, 6, 4, 1, PEER_MAC, PEER_IP, bytes(6), GUEST_IP)
    peer.sendall(framed(request.ljust(60, b"\0")))
    reply = recv_frame(reader)
    expected = PEER_MAC + GUEST_MAC + b"\x08\x06"
    expected += struct.pack("!HHBBH6s4s6s4s", 1, 0x0800, 6, 4, 2, GUEST_MAC, GUEST_IP, PEER_MAC, PEER_IP)
    if reply[:len(expected)] != expected:
        raise RuntimeError("ARP reply did not match the guest identity")


def check_reply(data, expected):
    if data[:14] != PEER_MAC + GUEST_MAC + b"\x08\x00" or data[14] != 0x45:
        raise RuntimeError("bad guest Ethernet or IPv4 header")
    ip = data[14:34]
    if ip[9] != 17 or ip[12:] != GUEST_IP + PEER_IP or checksum(ip):
        raise RuntimeError("bad guest IPv4 addresses, checksum, or protocol")
    ip_size = int.from_bytes(ip[2:4], "big")
    udp = data[34:14 + ip_size]
    src, dst, length, check = struct.unpack_from("!HHHH", udp)
    pseudo = GUEST_IP + PEER_IP + struct.pack("!BBH", 0, 17, length)
    if (src, dst, length) != (PORT, PORT, len(udp)) or not check or checksum(pseudo + udp):
        raise RuntimeError("bad guest UDP header or checksum")
    if udp[8:] != expected:
        raise RuntimeError(f"guest UDP payload mismatch for sequence {int.from_bytes(expected[:4], 'little')}")


def run(count, logs):
    logs.mkdir(parents=True, exist_ok=True)
    capture = logs / "ethernet.pcap"
    kernel = Path("build/stratum.elf")
    kernel_hash, boot_hash = file_hash(kernel), file_hash(BOOT_IMAGE)
    elapsed = time.monotonic()
    payloads = sha256()
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        listener.listen(1)
        listener.settimeout(20)
        port = listener.getsockname()[1]
        cmd = command(BOOT_IMAGE, cpus=2, ethernet=f"connect=127.0.0.1:{port}", capture=capture)
        with running(cmd, logs / "guest.log") as guest:
            peer, _ = listener.accept()
            with peer, peer.makefile("rb") as reader:
                peer.settimeout(10)
                peer.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                guest.expect(PROMPT)
                arp_exchange(peer, reader)
                guest.send(f"/bin/net-campaign {count}\r".encode())
                guest.expect(f"net-campaign: ready for {count} datagrams")
                for start in range(0, count, WINDOW):
                    group = [expected_payload(i) for i in range(start, min(count, start + WINDOW))]
                    peer.sendall(b"".join(framed(frame(p, i)) for i, p in enumerate(group, start)))
                    for payload in group:
                        check_reply(recv_frame(reader), payload)
                        payloads.update(payload)
                    if (start + WINDOW) % 25000 == 0:
                        print(f"Verified {(start + WINDOW)} UDP echoes on Ethernet", flush=True)
                result = guest.expect(PROMPT)
                require(f"[PASS] UDP echoes={count} exact payloads".encode(), result)
    stats = re.findall(rb"\[QUEUE\] (rx|tx) submitted=(\d+) completed=(\d+) available_wraps=(\d+) used_wraps=(\d+) free=(\d+) capacity=(\d+)", result)
    if [row[0] for row in stats] != [b"rx", b"tx"]:
        raise RuntimeError("missing RX or TX ring observations")
    for name, sent, completed, avail, used, free, capacity in stats:
        if int(sent) != count or int(completed) != count or int(free) > int(capacity):
            raise RuntimeError(f"{name!r}: ring capacity or completion mismatch")
        if count == REQUIRED and (int(avail), int(used)) != (2, 2):
            raise RuntimeError(f"{name!r}: expected two wraps of each index")
    if (file_hash(kernel), file_hash(BOOT_IMAGE)) != (kernel_hash, boot_hash):
        raise RuntimeError("kernel or boot image changed during the campaign")
    report = {
        "datagrams_per_direction": count, "payload_sha256": payloads.hexdigest(),
        "pcap_sha256": file_hash(capture), "kernel_sha256": kernel_hash,
        "boot_image_sha256": boot_hash, "seconds": round(time.monotonic() - elapsed, 3),
        "queues": [[row[0].decode(), *map(int, row[1:])] for row in stats],
    }
    (logs / "results.json").write_text(json.dumps(report, indent=2) + "\n")
    print(f"[PASS] {count} exact UDP datagrams in each direction, with RX/TX ring counters")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--count", type=int, default=REQUIRED)
    args = parser.parse_args()
    if not 1 <= args.count <= REQUIRED:
        parser.error(f"--count must be between 1 and {REQUIRED}")
    run(args.count, Path("build/net-wrap" if args.count == REQUIRED else "build/net-wrap-probe"))
