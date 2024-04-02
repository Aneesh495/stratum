#!/usr/bin/env python3
"""Check the compiler, image tools, emulator, and UEFI firmware."""

from hashlib import sha256
from pathlib import Path
import subprocess
import sys

from vm import firmware, tool


def main():
    failures = 0
    for name, flag in (("clang", "--version"), ("lld", "--version"),
                       ("ld.lld", "--version"), ("nasm", "-v"),
                       ("mformat", "--version"), ("mcopy", "--version"),
                       ("mmd", "--version"), ("qemu-system-x86_64", "--version")):
        binary = tool(name)
        args = [binary, "-flavor", "gnu", flag] if name == "lld" else [binary, flag]
        try:
            result = subprocess.run(args, capture_output=True, text=True, check=True, timeout=10)
            lines = (result.stdout or result.stderr).strip().splitlines()
            print(f"[OK] {binary} ({lines[0] if lines else 'available'})")
        except (OSError, subprocess.SubprocessError) as error:
            print(f"[FAIL] {binary}: {error}")
            failures += 1

    path = Path(firmware())
    if not path.is_file():
        print(f"[FAIL] Firmware missing at {path}. Set OVMF to a readable x86-64 OVMF code image.")
        failures += 1
    else:
        print(f"[OK] {path} (SHA-256 {sha256(path.read_bytes()).hexdigest()})")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
