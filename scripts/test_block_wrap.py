#!/usr/bin/env python3
"""Observe 150,000 successful virtio-blk requests and two full ring wraps."""

from pathlib import Path
import tempfile

from vm import BOOT_IMAGE, boot_profile, command, fresh_disk, running


def main():
    with fresh_disk() as disk, tempfile.TemporaryDirectory(dir="build") as directory:
        pattern = bytes((i * 17 + (i >> 8)) & 255 for i in range(4096))
        with disk.open("r+b") as image:
            image.write(pattern)
        boot = boot_profile(BOOT_IMAGE, Path(directory), "driver=blk-wrap")
        with running(command(boot, disk, cpus=2), Path("build/drivers/blk-wrap.log")) as guest:
            guest.expect("Block campaign complete: 150000 reads, 2 available wraps, 2 used wraps.", timeout=240)
        with disk.open("rb") as image:
            if image.read(4096) != pattern:
                raise RuntimeError("read-only campaign modified the payload")
    print("[PASS] 150,000 real virtio-blk reads, exact fixture bytes, and two full queue wraps.")


if __name__ == "__main__":
    main()
