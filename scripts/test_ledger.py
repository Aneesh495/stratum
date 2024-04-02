#!/usr/bin/env python3
"""Verify a ring-3 TCP ledger against an independent client and a rebooted disk."""

from pathlib import Path
import socket

from test_smoke import PROMPT
from vm import BOOT_IMAGE, command, fresh_disk, running


def start_ledger(guest):
    guest.expect(PROMPT)
    guest.send(b"ledgerd\r")
    guest.expect("ledgerd listening on port 9090.")


def request(port, parts, expected):
    with socket.create_connection(("127.0.0.1", port), timeout=5) as client:
        for part in parts:
            client.sendall(part)
        client.shutdown(socket.SHUT_WR)
        result = b""
        while len(result) <= 256:
            chunk = client.recv(256)
            if not chunk:
                break
            result += chunk
        if result != expected:
            raise RuntimeError(f"expected reply {expected!r}, received {result!r}")


def main():
    log = Path("build/ledger")
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        port = listener.getsockname()[1]
    with fresh_disk() as disk:
        cmd = command(BOOT_IMAGE, disk, cpus=4, host_port=port)
        with running(cmd, log / "before-stop.log") as guest:
            start_ledger(guest)
            request(port, [b"APPEND check-1 ", b"payload from client\n"], b"OK durable\n")
            request(port, [b"APPEND check-1 payload from client\n"], b"OK durable\n")
            request(port, [b"APPEND check-1 contradictory\n"], b"ERR conflict\n")
            request(port, [b"GET ../stratum\n"], b"ERR syntax\n")
            guest.interrupt()
        with running(cmd, log / "after-restart.log") as guest:
            start_ledger(guest)
            request(port, [b"GET check-1\n"], b"OK payload from client\n")
            request(port, [b"GET absent\n"], b"ERR missing\n")
            request(port, [b"APPEND check-2 new record\n"], b"OK durable\n")
            request(port, [b"GET check-2\n"], b"OK new record\n")
    print("[PASS] TCP ledger requests, duplicate/conflict handling, and durability across VM interruption")


if __name__ == "__main__":
    main()
