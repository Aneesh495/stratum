#!/usr/bin/env python3
"""Run mixed filesystem operations and independently inspect the recovered namespace."""

from pathlib import Path
import mmap
import struct
import subprocess

from test_smoke import PROMPT, require, shell
from vm import BOOT_IMAGE, command, fresh_disk, running, tool


BLOCK = 4096
INODE_TABLE = 4 * BLOCK
INODE_SIZE = 128
DIRENT_SIZE = 256


def inode_field(inode, offset):
    return INODE_TABLE + INODE_SIZE * inode + offset


def directory(image, inode):
    block, = struct.unpack_from("<I", image, inode_field(inode, 48))
    entries = {}
    for offset in range(block * BLOCK, (block + 1) * BLOCK, DIRENT_SIZE):
        target, _, size, _ = struct.unpack_from("<IHBB", image, offset)
        if target:
            entries[image[offset + 8:offset + 8 + size].decode()] = (target, offset)
    return entries


def dirent(inode, name):
    return struct.pack("<IHBB248s", inode, DIRENT_SIZE, len(name), 2, name.encode())


def verify_corruptions(disk, checker, logs):
    with disk.open("r+b") as stream, mmap.mmap(stream.fileno(), 0) as image:
        entries = directory(image, 1)
        from_ino, from_entry = entries["from"]
        to_ino, to_entry = entries["to"]
        done_ino, done_entry = entries["done"]
        from_dir, to_dir = directory(image, from_ino), directory(image, to_ino)
        _, from_dotdot = from_dir[".."]
        _, to_dotdot = to_dir[".."]

        def check(name, message, updates):
            saved = [(offset, image[offset:offset + len(data)]) for offset, data in updates]
            try:
                for offset, data in updates:
                    image[offset:offset + len(data)] = data
                image.flush()
                result = subprocess.run([checker, str(disk)], capture_output=True, timeout=10)
                output = result.stdout + result.stderr
                (logs / f"rejected-{name}.log").write_bytes(output)
                if result.returncode != 1:
                    raise RuntimeError(f"checker accepted {name}")
                require(message, output)
            finally:
                for offset, data in saved:
                    image[offset:offset + len(data)] = data
                image.flush()

        check("duplicate-name", b"duplicate name", [(to_entry + 6, b"\x04"), (to_entry + 8, b"from")])
        check("orphan", b"invalid namespace parent", [(done_entry, bytes(DIRENT_SIZE))])
        check("type", b"type mismatch", [(done_entry + 7, b"\x02")])
        check("wrong-parent", b"invalid namespace parent", [(from_dotdot, struct.pack("<I", to_ino))])
        check("wrong-block-count", b"block count disagrees", [(inode_field(done_ino, 16), bytes(8))])
        check("cycle", b"namespace cycle", [
            (from_entry, bytes(DIRENT_SIZE)), (to_entry, bytes(DIRENT_SIZE)),
            (from_dotdot, struct.pack("<I", to_ino)), (to_dotdot, struct.pack("<I", from_ino)),
            (from_dotdot + DIRENT_SIZE, dirent(to_ino, "to")),
            (to_dotdot + DIRENT_SIZE, dirent(from_ino, "from")),
            (inode_field(1, 6), struct.pack("<H", 2)),
            (inode_field(from_ino, 6), struct.pack("<H", 3)),
            (inode_field(to_ino, 6), struct.pack("<H", 3)),
        ])


def main():
    logs = Path("build/storage-mixed")
    logs.mkdir(parents=True, exist_ok=True)
    checker = str(logs / "strata_check")
    subprocess.run([tool("clang"), "-Wall", "-Wextra", "-O2",
                    "tools/strata_check.c", "-o", checker], check=True, timeout=30)
    with fresh_disk() as disk:
        cmd = command(BOOT_IMAGE, disk, cpus=4)
        with running(cmd, logs / "workload.log") as guest:
            guest.expect(PROMPT)
            guest.send(b"/bin/fs-campaign\r")
            for rounds in (1000, 2000, 3000, 4000):
                guest.expect(f"fs-campaign: rounds={rounds} operations={rounds * 32 + 2}", timeout=180)
                print(f"Verified {rounds} mixed filesystem rounds", flush=True)
            guest.expect("[PASS] StrataFS mixed operations=128006 rounds=4000 durable fixture saved", timeout=10)
            guest.interrupt()
        with running(cmd, logs / "recovered.log") as guest:
            guest.expect(PROMPT)
            if shell(guest, "cat /done") != b"stratum-storage-verified":
                raise RuntimeError("durable fixture changed after reboot")
        with (logs / "checker.log").open("wb") as log:
            subprocess.run([checker, str(disk)], check=True, timeout=10, stdout=log, stderr=subprocess.STDOUT)
        verify_corruptions(disk, checker, logs)
        subprocess.run([checker, str(disk)], check=True, timeout=10)
    print("[PASS] 128006 mixed filesystem operations, rebooted fixture, and six rejected namespace corruptions")


if __name__ == "__main__":
    main()
