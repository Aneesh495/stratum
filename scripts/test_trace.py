#!/usr/bin/env python3
"""Verify a native trace snapshot, overflow reporting and shell export."""

from hashlib import sha256
from pathlib import Path
import re

from test_smoke import PROMPT, require, shell
from vm import BOOT_IMAGE, command, fresh_disk, running


def main():
    with fresh_disk() as disk:
        with running(command(BOOT_IMAGE, disk, cpus=4), Path("build/trace/guest.log")) as guest:
            guest.expect(PROMPT)
            output = shell(guest, "/bin/trace-campaign")
            result = re.search(rb"trace overflow cpu=(\d+) retained=1024 lost=(\d+) verified=(\d+) newest=4095", output)
            if not result or int(result[1]) >= 4 or int(result[2]) < 3072 or int(result[3]) < 900:
                raise RuntimeError(f"missing trace overflow evidence: {output!r}")
            snapshot = shell(guest, f"trace {int(result[1])}")
            require(f"Trace CPU {int(result[1])} ".encode(), snapshot)
            require(b" event=2 ", snapshot)
            if shell(guest, "echo stratum | sha256sum") != sha256(b"stratum\n").hexdigest().encode() + b"\r\n":
                raise RuntimeError("native checksum disagrees with host SHA-256")
            if shell(guest, "write /original stratum") or shell(guest, "cp /original /copy"):
                raise RuntimeError("copy fixture setup failed")
            if shell(guest, "cat /copy") != b"stratum\r\n":
                raise RuntimeError("native copy changed file bytes")
    print("[PASS] Native trace export and overflow control, SHA-256 and file copy")


if __name__ == "__main__":
    main()
