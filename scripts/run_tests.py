#!/usr/bin/env python3
"""Run the deterministic host models with fatal sanitizer diagnostics."""

from pathlib import Path
import subprocess

from vm import tool


def main():
    Path("build").mkdir(exist_ok=True)
    for source in ("tests/host/test_mm_model.c", "tests/host/test_models.c", "tests/faults/test_faults.c"):
        binary = Path("build") / Path(source).stem
        print(f"Running {source}", flush=True)
        subprocess.run([tool("clang"), "-std=c17", "-O1", "-g", "-Wall", "-Wextra",
                        "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                        source, "-o", binary], check=True, timeout=60)
        subprocess.run([binary], check=True, timeout=60)


if __name__ == "__main__":
    main()
