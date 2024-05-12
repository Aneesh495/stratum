#!/usr/bin/env python3
"""Exercise virtio-blk saturation, PCI resets, and backend I/O errors in QEMU."""

import errno
from pathlib import Path
import tempfile

from vm import BOOT_IMAGE, boot_profile, command, fresh_disk, running


def main():
    with fresh_disk() as disk, tempfile.TemporaryDirectory(dir="build") as directory:
        pattern = bytes((i * 17 + (i >> 8)) & 255 for i in range(512))
        with disk.open("r+b") as image:
            image.write(pattern)
        root = Path(directory)
        rules = root / "block-errors.conf"
        rules.write_text(f'[inject-error]\nevent = "read_aio"\nerrno = "{errno.EIO}"\nsector = "64"\n',
                         encoding="ascii")
        boot = boot_profile(BOOT_IMAGE, root, "driver=blk-errors")
        cmd = command(boot, disk, cpus=2, block_debug=rules)
        with running(cmd, Path("build/drivers/blk-errors.log")) as guest:
            guest.expect("Block saturation: 500 cases with completed device reads and full reclamation.", timeout=240)
            guest.expect("Block reset: 500 cases with old queues retired and restored device capacity.", timeout=180)
            guest.expect("Block I/O errors: 1000 device failures with continued reads and no descriptor leaks.", timeout=180)
        with disk.open("rb") as image:
            if image.read(512) != pattern:
                raise RuntimeError("driver campaign modified the payload")
    print("[PASS] 500 real queue saturations, 500 PCI resets, and 1000 device I/O errors.")


if __name__ == "__main__":
    main()
