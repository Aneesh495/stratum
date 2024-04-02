#!/usr/bin/env python3
"""
scripts/run_fuzz.py - Exercises six standalone parser models under sanitizers.
Compiles tests/fuzz/test_fuzz.c with AddressSanitizer and UndefinedBehaviorSanitizer,
running 2,100,000 executions across all 6 parser/decoder targets.
"""

import os
import subprocess
import sys

from vm import tool

PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FUZZ_SRC = os.path.join(PROJECT_ROOT, "tests", "fuzz", "test_fuzz.c")
FUZZ_BIN = os.path.join(PROJECT_ROOT, "build", "test_fuzz")


def main():
    os.makedirs(os.path.dirname(FUZZ_BIN), exist_ok=True)
    clang = tool("clang")

    print("=== Compiling Stratum Fuzzing Suite with ASan & UBSan ===")
    compile_cmd = [
        clang, "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-g", "-O2",
        "-Wall", "-Wextra",
        FUZZ_SRC, "-o", FUZZ_BIN
    ]
    res = subprocess.run(compile_cmd, cwd=PROJECT_ROOT, timeout=120)
    if res.returncode != 0:
        print("[FAIL] Failed to compile fuzz suite with sanitizers.")
        return 1

    print("=== Executing 2,100,000 Fuzz Executions Across 6 Targets ===")
    res = subprocess.run([FUZZ_BIN, "350000"], cwd=PROJECT_ROOT, timeout=120)
    if res.returncode != 0:
        print(f"[FAIL] Fuzz suite failed with exit code {res.returncode}")
        return res.returncode

    print("[OK] Host parser-model mutation checks passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
