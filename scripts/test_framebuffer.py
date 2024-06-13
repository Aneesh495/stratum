#!/usr/bin/env python3
"""Exercise the PS/2 IRQ and compare captured VGA pixels with a glyph fixture."""

import os
from pathlib import Path
import tempfile
import time

from qmp import monitor
from test_smoke import PROMPT, require, shell
from vm import BOOT_IMAGE, boot_profile, command, fresh_disk, running


GLYPH = (0x00, 0xFF, 0xC3, 0x06, 0x0C, 0x18, 0x30, 0x60,
         0xC0, 0xC3, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00)


def contains_glyph_row(path):
    magic, dimensions, maximum, pixels = path.read_bytes().split(b"\n", 3)
    width, height = map(int, dimensions.split())
    if magic != b"P6" or maximum != b"255" or len(pixels) != width * height * 3:
        raise RuntimeError("invalid QEMU framebuffer capture")
    for y in range(0, height - 15, 16):
        for x in range(0, width - 63, 8):
            top = (y * width + x) * 3
            background = pixels[top:top + 3]
            foreground = pixels[top + width * 3:top + width * 3 + 3]
            if background == foreground:
                continue
            for row, mask in enumerate(GLYPH):
                offset = top + row * width * 3
                expected = b"".join(foreground if mask & (0x80 >> bit) else background
                                    for bit in range(8)) * 8
                if pixels[offset:offset + 192] != expected:
                    break
            else:
                return True
    return False


def main():
    logs = Path("build/framebuffer").resolve()
    logs.mkdir(parents=True, exist_ok=True)
    before, after = logs / "before.ppm", logs / "after.ppm"
    with tempfile.TemporaryDirectory(prefix="fb-", dir="build") as work, fresh_disk() as disk:
        boot = boot_profile(BOOT_IMAGE, Path(work), "console=both")
        path = os.path.relpath(Path(work) / "monitor")
        cmd = command(boot, disk, cpus=2) + ["-qmp", f"unix:{path},server=on,wait=off"]
        with running(cmd, logs / "guest.log") as guest, monitor(path, logs / "qmp.jsonl") as qmp:
            guest.expect(PROMPT)
            qmp("screendump", {"filename": str(before)})
            for key in ("h", "e", "x", "backspace", "l", "p", "ret"):
                qmp("send-key", {"keys": [{"type": "qcode", "data": key}], "hold-time": 30})
                time.sleep(0.08)
            reply = guest.expect(PROMPT, timeout=10)
            require(b"help", reply)
            require(b"sysinfo", reply)
            if shell(guest, "echo ZZZZZZZZ") != b"ZZZZZZZZ\r\n":
                raise RuntimeError("mirrored console changed command output")
            qmp("screendump", {"filename": str(after)})
    if contains_glyph_row(before) or not contains_glyph_row(after):
        raise RuntimeError("framebuffer pixels disagree with the newly rendered fixture")
    print("[PASS] PS/2 key input, editing and exact 8x16 framebuffer glyphs")


if __name__ == "__main__":
    main()
