#!/usr/bin/env python3
"""Replace a real shared kernel mapping 10,000 times across all guest CPUs."""

from pathlib import Path
import tempfile

from test_smoke import PROMPT
from vm import BOOT_IMAGE, boot_profile, command, running


def main():
    with tempfile.TemporaryDirectory(dir="build") as directory:
        boot = boot_profile(BOOT_IMAGE, Path(directory), "memory=tlb")
        for cpus in (4, 8):
            with running(command(boot, cpus=cpus), Path(f"build/tlb/{cpus}cpu.log")) as guest:
                guest.expect(f"[TLB] cycles=10000 CPUs={cpus} retired frames verified, final unmap acknowledged.",
                             timeout=180)
                guest.expect(PROMPT)
            print(f"[PASS] {cpus} CPUs: 10000 mapping replacements with acknowledged invalidation and frame reuse",
                  flush=True)


if __name__ == "__main__":
    main()
