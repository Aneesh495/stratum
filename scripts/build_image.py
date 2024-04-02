#!/usr/bin/env python3
"""Build an EFI System Partition image and a pristine data disk template."""

import argparse
from hashlib import sha256
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile

from vm import tool


IMAGE_BYTES = 64 * 1024 * 1024
EPOCH = 1700000000


def mtools(*args):
    env = os.environ | {"SOURCE_DATE_EPOCH": str(EPOCH), "TZ": "UTC", "MTOOLSRC": os.devnull}
    subprocess.run(args, env=env, check=True, timeout=30)


def build_boot_disk(image, loader, kernel, initramfs):
    with image.open("wb") as disk:
        disk.truncate(IMAGE_BYTES)
        disk.seek(446)
        start_lba = 2048
        partition = struct.pack("<BBBBBBBBII", 0x80, 0, 2, 0, 0xEF, 255, 255, 255,
                                start_lba, IMAGE_BYTES // 512 - start_lba)
        disk.write(partition + bytes(48) + b"\x55\xAA")

    esp = f"{image}@@1M"
    mtools(tool("mformat"), "-i", esp, "-F", "-N", "12345678", "::")
    mtools(tool("mmd"), "-i", esp, "::/EFI")
    mtools(tool("mmd"), "-i", esp, "::/EFI/BOOT")
    files = [
        (loader, "::/EFI/BOOT/BOOTX64.EFI"),
        (kernel, "::/stratum.elf"),
        (initramfs, "::/initramfs.cpio"),
    ]
    for source, dest in files:
        staged = image.parent / dest.rsplit("/", 1)[1]
        shutil.copyfile(source, staged)
        os.utime(staged, (EPOCH, EPOCH))
        mtools(tool("mcopy"), "-m", "-i", esp, staged, dest)

    startup = image.parent / "startup.nsh"
    startup.write_bytes(b"\\EFI\\BOOT\\BOOTX64.EFI\r\n")
    os.utime(startup, (EPOCH, EPOCH))
    mtools(tool("mcopy"), "-m", "-i", esp, startup, "::/startup.nsh")


def write_image(path, builder):
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=path.parent) as directory:
        staged = Path(directory) / path.name
        builder(staged)
        with staged.open("rb") as image:
            h = sha256()
            while chunk := image.read(65536):
                h.update(chunk)
        staged.replace(path)
    print(f"[OK] {path} (SHA-256 {h.hexdigest()})")


def blank_disk(path):
    with path.open("wb") as image:
        image.truncate(IMAGE_BYTES)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for flag in ("boot-img", "data-img", "loader", "kernel", "initramfs"):
        parser.add_argument("--" + flag, required=True, type=Path)
    args = parser.parse_args()
    write_image(args.boot_img, lambda staged: build_boot_disk(staged, args.loader, args.kernel, args.initramfs))
    write_image(args.data_img, blank_disk)


if __name__ == "__main__":
    main()
