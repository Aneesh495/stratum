#!/usr/bin/env python3
"""Exercise real low-memory slab rollback and TCP socket capacity reuse."""

from pathlib import Path
import re
import tempfile

from test_smoke import PROMPT, require, shell
from vm import BOOT_IMAGE, boot_profile, command, running


def check_slab(raw):
    observed = re.search(rb"slab classes=8 objects=256 physical_pressure=(\d+) recovered=(\d+)", raw)
    if not observed or int(observed[1]) <= 256 or int(observed[2]) <= int(observed[1]):
        raise RuntimeError("missing or invalid allocator-pressure observation")


def main():
    logs = Path("build/resource-limits")
    with tempfile.TemporaryDirectory(dir="build") as work:
        image = boot_profile(BOOT_IMAGE, Path(work), "memory=slab-pressure")
        with running(command(image, cpus=1, memory="64M"), logs / "slab.log") as guest:
            check_slab(guest.expect(PROMPT))
    with running(command(BOOT_IMAGE, cpus=4), logs / "sockets.log") as guest:
        guest.expect(PROMPT)
        require(b"TCP sockets=128 ENOBUFS=2 capacity recycled and resources reconciled",
                shell(guest, "/bin/net-campaign limits"))
    print("[PASS] Real slab pressure and recovery, TCP socket exhaustion and reuse")


if __name__ == "__main__":
    main()
