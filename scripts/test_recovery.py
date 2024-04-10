#!/usr/bin/env python3
"""Interrupt a published WAL transaction before home writes, then test replay and rejection."""

from hashlib import sha256
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile

from test_smoke import PROMPT, require, shell
from vm import BOOT_IMAGE, boot_profile, command, fresh_disk, running, tool


def digest(path):
    with path.open("rb") as data:
        h = sha256()
        while chunk := data.read(65536):
            h.update(chunk)
        return h.hexdigest()


def corrupt_payload(disk):
    block_size = 4096
    journal_start = 68
    with disk.open("r+b") as image:
        image.seek(journal_start * block_size + 16)
        head, tail = struct.unpack("<II", image.read(8))
        if head == tail:
            raise RuntimeError("interruption did not leave a published WAL range")
        payload = 1 if head == 511 else head + 1
        image.seek((journal_start + payload) * block_size + 64)
        before = image.read(1)
        image.seek(-1, 1)
        image.write(bytes([before[0] ^ 0x80]))


def main():
    log = Path("build/recovery")
    checker = "build/strata_check"
    subprocess.run([tool("clang"), "-Wall", "-Wextra", "-O2",
                    "tools/strata_check.c", "-o", checker], check=True, timeout=30)
    with fresh_disk() as disk, tempfile.TemporaryDirectory(dir="build") as directory:
        directory = Path(directory)
        normal = command(BOOT_IMAGE, disk, cpus=2)
        with running(normal, log / "setup.log") as guest:
            guest.expect(PROMPT)
            if shell(guest, "write /wal-token before"):
                raise RuntimeError("setup write failed")

        fault_image = boot_profile(BOOT_IMAGE, directory, "crash=wal-published")
        with running(command(fault_image, disk, cpus=2), log / "interrupted.log") as guest:
            guest.expect(PROMPT)
            guest.send(b"write /wal-token recovered\r")
            guest.expect("[TEST] WAL crash stage published.")
            guest.interrupt()

        corrupt = directory / "corrupt.img"
        shutil.copyfile(disk, corrupt)
        corrupt_payload(corrupt)
        before = digest(corrupt)
        with running(command(BOOT_IMAGE, corrupt, cpus=2), log / "rejected.log") as guest:
            require(b"Journal initialization failed: -5", guest.expect(PROMPT))
            require(b"err=-2", shell(guest, "cat /wal-token"))
        if digest(corrupt) != before:
            raise RuntimeError("mount modified corrupt journal image")

        with running(normal, log / "replayed.log") as guest:
            require(b"[JOURNAL] Replayed committed transaction", guest.expect(PROMPT))
            if shell(guest, "cat /wal-token") != b"before\r\nrecovered\r\n":
                raise RuntimeError("replayed file has unexpected bytes")
        subprocess.run([checker, str(disk)], check=True, timeout=10)

        bad_superblock = directory / "bad-superblock.img"
        shutil.copyfile(disk, bad_superblock)
        with bad_superblock.open("r+b") as image:
            image.seek(4096)
            image.write(b"\x00")
        before = digest(bad_superblock)
        with running(command(BOOT_IMAGE, bad_superblock, cpus=2), log / "bad-superblock.log") as guest:
            require(b"Invalid superblock. Refusing to format", guest.expect(PROMPT))
        if digest(bad_superblock) != before:
            raise RuntimeError("mount modified volume with a bad superblock")
        result = subprocess.run([checker, str(bad_superblock)], timeout=10)
        if result.returncode != 1:
            raise RuntimeError("offline checker accepted corrupt superblock")

    print("[PASS] WAL replay, checksum rejection, and preservation of corrupt volumes")


if __name__ == "__main__":
    main()
