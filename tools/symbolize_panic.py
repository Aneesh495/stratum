#!/usr/bin/env python3
"""Resolve observed kernel instruction addresses against an exact ELF64 image."""

import argparse
from hashlib import sha256
import json
from pathlib import Path
import re
import struct


SECTION = struct.Struct("<IIQQQQIIQQ")
PROGRAM = struct.Struct("<IIQQQQQQ")
SYMBOL = struct.Struct("<IBBHQQ")


def symbolize(kernel, transcript):
    image = kernel.read_bytes()
    if image[:7] != b"\x7fELF\x02\x01\x01" or len(image) < 64:
        raise ValueError("expected a little-endian ELF64 image")
    machine, = struct.unpack_from("<H", image, 18)
    phoff, shoff = struct.unpack_from("<QQ", image, 32)
    phsize, phnum, shsize, shnum = struct.unpack_from("<HHHH", image, 54)
    if (machine, phsize, shsize) != (62, PROGRAM.size, SECTION.size) or (
        phoff + phsize * phnum > len(image) or shoff + shsize * shnum > len(image)
    ):
        raise ValueError("invalid x86-64 ELF table geometry")

    def content(offset, size):
        if offset + size > len(image):
            raise ValueError("ELF table extends past the file")
        return image[offset:offset + size]

    sections = list(SECTION.iter_unpack(content(shoff, shsize * shnum)))
    symbols = []
    for _, kind, _, _, offset, size, strings_index, _, _, entsize in sections:
        if kind != 2:
            continue
        if entsize != SYMBOL.size or size % SYMBOL.size or strings_index >= len(sections):
            raise ValueError("invalid ELF symbol table")
        strings = sections[strings_index]
        if strings[1] != 3:
            raise ValueError("invalid ELF symbol string table")
        names = content(strings[4], strings[5])
        for name, info, _, index, start, length in SYMBOL.iter_unpack(content(offset, size)):
            if info & 15 != 2 or not 0 < index < len(sections) or not length:
                continue
            end = names.find(b"\0", name)
            if end < 0:
                raise ValueError("invalid ELF symbol name")
            symbols.append((start, length, names[name:end].decode("ascii")))
    executable = [
        (vaddr, offset, filesz) for kind, flags, offset, vaddr, _, filesz, _, _
        in PROGRAM.iter_unpack(content(phoff, phsize * phnum)) if kind == 1 and flags & 1
    ]
    raw = transcript.read_bytes()
    addresses = dict.fromkeys(int(value, 16) for value in re.findall(rb"\bRIP:\s+(0x[\da-fA-F]+)", raw))
    if not addresses:
        raise ValueError("transcript contains no RIP observations")
    frames = []
    for address in addresses:
        matches = [entry for entry in symbols if entry[0] <= address < entry[0] + entry[1]]
        location = next((offset + address - start for start, offset, length in executable
                         if start <= address and address + 2 <= start + length), None)
        if not matches or location is None:
            raise ValueError(f"no executable symbol at {address:#x}")
        start, _, name = min(matches, key=lambda entry: entry[1])
        frames.append({"rip": f"{address:#x}", "symbol": name, "offset": address - start,
                       "opcode": content(location, 2).hex()})
    return {"kernel_sha256": sha256(image).hexdigest(), "transcript_sha256": sha256(raw).hexdigest(),
            "frames": frames}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("kernel", type=Path)
    parser.add_argument("transcript", type=Path)
    args = parser.parse_args()
    print(json.dumps(symbolize(args.kernel, args.transcript), indent=2))


if __name__ == "__main__":
    main()
