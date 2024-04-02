#!/usr/bin/env python3
"""Boot the normal image and exercise targeted malformed EFI loader inputs."""

from pathlib import Path
import shutil
import struct
import subprocess
import tempfile

from test_smoke import PROMPT
from vm import BOOT_IMAGE, command, running, tool


def replace(data, offset, fmt, value):
    mutated = bytearray(data)
    struct.pack_into("<" + fmt, mutated, offset, value)
    return mutated


def cases(kernel):
    phoff = struct.unpack_from("<Q", kernel, 32)[0]
    filesz = struct.unpack_from("<Q", kernel, phoff + 32)[0]
    return [
        ("magic", replace(kernel, 0, "I", 0), "header validation failed"),
        ("class", replace(kernel, 4, "B", 1), "header validation failed"),
        ("endianness", replace(kernel, 5, "B", 2), "header validation failed"),
        ("ident-version", replace(kernel, 6, "B", 0), "header validation failed"),
        ("dynamic", replace(kernel, 16, "H", 3), "header validation failed"),
        ("machine", replace(kernel, 18, "H", 183), "header validation failed"),
        ("version", replace(kernel, 20, "I", 0), "header validation failed"),
        ("phoff-overflow", replace(kernel, 32, "Q", 2**64 - 32), "header validation failed"),
        ("header-size", replace(kernel, 52, "H", 1), "header validation failed"),
        ("program-header-size", replace(kernel, 54, "H", 1), "header validation failed"),
        ("no-program-headers", replace(kernel, 56, "H", 0), "header validation failed"),
        ("truncated-header", kernel[:32], "Failed to read kernel ELF header"),
        ("entry-outside-executable", replace(kernel, 24, "Q", 0x400000), "Failed to load kernel ELF segments"),
        ("writable-executable", replace(kernel, phoff + 4, "I", 7), "Failed to load kernel ELF segments"),
        ("segment-offset", replace(kernel, phoff + 8, "Q", 2**64 - 1), "Failed to load kernel ELF segments"),
        ("virtual-address", replace(kernel, phoff + 16, "Q", 2**64 - 8), "Failed to load kernel ELF segments"),
        ("file-larger-than-memory", replace(kernel, phoff + 40, "Q", filesz - 1), "Failed to load kernel ELF segments"),
    ]


def main():
    logs = Path("build/boot")
    with running(command(BOOT_IMAGE, cpus=2), logs / "no-disk.log") as guest:
        reply = guest.expect(PROMPT)
        if b"Root mount failed" not in reply:
            raise RuntimeError("missing no-disk diagnostic")

    kernel = Path("build/stratum.elf").read_bytes()
    with tempfile.TemporaryDirectory(dir="build") as directory:
        image = Path(directory) / BOOT_IMAGE.name
        mutated = Path(directory) / "stratum.elf"
        for name, data, error in cases(kernel):
            shutil.copyfile(BOOT_IMAGE, image)
            mutated.write_bytes(data)
            subprocess.run([tool("mcopy"), "-o", "-i", f"{image}@@1M",
                            str(mutated), "::/stratum.elf"], check=True, timeout=10)
            with running(command(image, cpus=1), logs / f"{name}.log") as guest:
                transcript = guest.expect(error)
                if b"Kernel entered at higher-half entry point" in transcript:
                    raise RuntimeError(f"loader entered the kernel for {name}")
            print(f"[PASS] rejected {name}", flush=True)


if __name__ == "__main__":
    main()
