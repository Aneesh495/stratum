#!/usr/bin/env python3
"""Observe device completion and native rejection of invalid Ethernet traffic."""

from collections import Counter
import json
from pathlib import Path
import socket
import struct
import tempfile

from evidence_format import file_hash
from test_net_wrap import arp_exchange, check_reply, checksum, expected_payload, frame, framed, recv_frame
from test_smoke import PROMPT, require
from vm import BOOT_IMAGE, command, running


CASES = (
    "ether-type", "ipv4-version", "ipv4-header-length", "ipv4-short-total", "ipv4-long-total",
    "ipv4-hop-limit", "ipv4-fragment", "ipv4-checksum", "udp-length", "udp-checksum",
)
INVALID = 10000
GROUP_SIZE = 5


def invalid_frame(sequence):
    packet = bytearray(frame(expected_payload(sequence), sequence))
    kind = sequence % len(CASES)
    if kind == 0:
        packet[12:14] = b"\x88\xb5"
    elif kind == 1:
        packet[14] = 0x65
    elif kind == 2:
        packet[14] = 0x44
    elif kind == 3:
        struct.pack_into("!H", packet, 16, 19)
    elif kind == 4:
        struct.pack_into("!H", packet, 16, len(packet))
    elif kind == 5:
        packet[22] = 0
    elif kind == 6:
        struct.pack_into("!H", packet, 20, 0x2000)
    elif kind == 7:
        packet[24] ^= 1
    elif kind == 8:
        struct.pack_into("!H", packet, 38, len(packet) - 33)
    else:
        stored, = struct.unpack_from("!H", packet, 40)
        struct.pack_into("!H", packet, 40, 1 if stored != 1 else 2)

    if 1 <= kind <= 6:
        struct.pack_into("!H", packet, 24, 0)
        struct.pack_into("!H", packet, 24, checksum(bytes(packet[14:34])))
    return bytes(packet)


def main():
    logs = Path(tempfile.mkdtemp(prefix="net-reject-", dir="build"))
    capture = logs / "ethernet.pcap"
    kernel = Path("build/stratum.elf")
    kernel_hash, boot_hash = file_hash(kernel), file_hash(BOOT_IMAGE)
    valid = INVALID // GROUP_SIZE
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
                guest.send(f"/bin/net-campaign {valid} {INVALID}\r".encode())
                guest.expect(f"net-campaign: ready for {valid} datagrams")
                counts = Counter()
                for i in range(valid):
                    first = i * GROUP_SIZE
                    cases = range(first, first + GROUP_SIZE)
                    expected = expected_payload(i)
                    peer.sendall(b"".join(framed(invalid_frame(case)) for case in cases) + framed(frame(expected, i)))
                    check_reply(recv_frame(reader), expected)
                    counts.update(CASES[case % len(CASES)] for case in cases)
                raw = guest.expect(PROMPT)
                require(f"[PASS] UDP echoes={valid} exact payloads".encode(), raw)
                require(f"[QUEUE] rx submitted={valid + INVALID} completed={valid + INVALID}".encode(), raw)
                require(f"[QUEUE] tx submitted={valid} completed={valid}".encode(), raw)
    if set(counts.values()) != {1000} or counts.keys() != set(CASES):
        raise RuntimeError("incomplete malformed-input coverage")
    if (file_hash(kernel), file_hash(BOOT_IMAGE)) != (kernel_hash, boot_hash):
        raise RuntimeError("image changed during the network campaign")
    report = {
        "cases": counts, "rejected": INVALID, "valid_echoes": valid,
        "pcap_sha256": file_hash(capture), "kernel_sha256": kernel_hash, "boot_image_sha256": boot_hash,
    }
    (logs / "results.json").write_text(json.dumps(report, indent=2) + "\n")
    print(f"[PASS] {INVALID} invalid frames across {len(CASES)} categories with {valid} verified valid echoes")


if __name__ == "__main__":
    main()
