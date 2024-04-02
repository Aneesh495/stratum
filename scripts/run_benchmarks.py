#!/usr/bin/env python3
"""Measure host-only model microbenchmarks and retain their raw output."""

from pathlib import Path
import subprocess

from vm import tool


def main():
    out = Path("build")
    out.mkdir(exist_ok=True)
    binary = out / "bench_suite"
    subprocess.run([tool("clang"), "-O3", "-Wall", "-Wextra",
                    "bench/bench_suite.c", "-o", binary], check=True, timeout=60)
    with (out / "host-benchmarks.log").open("w") as log:
        log.write("Host model microbenchmarks. QEMU and guest code are outside this measurement.\n")
        log.flush()
        subprocess.run([binary], stdout=log, stderr=subprocess.STDOUT, check=True, timeout=120)
    print((out / "host-benchmarks.log").read_text(), end="")


if __name__ == "__main__":
    main()
