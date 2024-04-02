#!/usr/bin/env python3
"""Launch the normal Stratum image with a persistent data disk."""

import argparse
from pathlib import Path
import os
import shutil
import subprocess
import tempfile

from vm import command


def initialize_disk(disk, template):
    if disk.exists():
        return
    disk.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=disk.parent) as directory:
        copy = Path(directory) / disk.name
        shutil.copyfile(template, copy)
        try:
            os.link(copy, disk)
        except FileExistsError:
            pass  # Another launch already initialized it.


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--boot-img", type=Path, required=True)
    parser.add_argument("--data-img", type=Path)
    parser.add_argument("--data-template", type=Path)
    parser.add_argument("--ovmf")
    parser.add_argument("--cpus", type=int, choices=(1, 2, 4, 8), default=4)
    parser.add_argument("--mem", default="256M")
    parser.add_argument("--gdb", type=int, default=0)
    parser.add_argument("--ledger-port", type=int)
    parser.add_argument("--timeout", type=float)
    args = parser.parse_args()
    if args.data_img is not None and args.data_template is not None:
        initialize_disk(args.data_img, args.data_template)

    cmd = command(args.boot_img, args.data_img, cpus=args.cpus, memory=args.mem,
                  ovmf=args.ovmf, host_port=args.ledger_port)
    # QEMU owns terminal input. Ctrl-A X exits while preserving the data disk.
    cmd += ["-serial", "mon:stdio"]
    if args.gdb:
        cmd += ["-gdb", f"tcp:127.0.0.1:{args.gdb}", "-S"]
    try:
        return subprocess.run(cmd, timeout=args.timeout).returncode
    except subprocess.TimeoutExpired:
        print("VM timed out.", flush=True)
        return 124


if __name__ == "__main__":
    raise SystemExit(main())
