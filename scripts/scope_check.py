#!/usr/bin/env python3
"""
scripts/scope_check.py - Reproducible line counting for Stratum OS.
Counts non-blank, non-comment authored production code in kernel/ and drivers.
Outputs private accounting ledger to .agent-local/scope_ledger.json.
"""

import json
import os
import sys

KERNEL_DIRS = [
    "kernel/arch/x86_64",
    "kernel/core",
    "kernel/mm",
    "kernel/sched",
    "kernel/sync",
    "kernel/proc",
    "kernel/ipc",
    "kernel/drivers",
    "kernel/block",
    "kernel/fs",
    "kernel/net",
    "kernel/debug",
    "kernel/ledger",
    "include/kernel",
    "include/shared",
]

ALLOWED_EXTENSIONS = {".c", ".h", ".s", ".S", ".asm"}


def is_production_file(filepath):
    _, ext = os.path.splitext(filepath)
    if ext not in ALLOWED_EXTENSIONS:
        return False
    # Exclude tests, mocks, fixtures, benchmarks
    parts = filepath.split(os.sep)
    for p in parts:
        if p in ("tests", "bench", "fixtures", "snapshots", "mock"):
            return False
    return True


def count_substantive_lines(filepath):
    lines = 0
    in_block_comment = False
    with open(filepath, "r", encoding="utf-8", errors="ignore") as f:
        for raw_line in f:
            line = raw_line.strip()
            if not line:
                continue

            if in_block_comment:
                if "*/" in line:
                    in_block_comment = False
                    after = line.split("*/", 1)[1].strip()
                    if after and not after.startswith("//"):
                        lines += 1
                continue

            if line.startswith("/*"):
                if "*/" in line:
                    after = line.split("*/", 1)[1].strip()
                    if after and not after.startswith("//"):
                        lines += 1
                else:
                    in_block_comment = True
                continue

            if line.startswith("//") or line.startswith(";"):
                continue

            lines += 1
    return lines


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    os.chdir(root)

    file_counts = {}
    total_lines = 0

    for kdir in KERNEL_DIRS:
        full_dir = os.path.join(root, kdir)
        if not os.path.exists(full_dir):
            continue
        for dirpath, _, filenames in os.walk(full_dir):
            for fname in sorted(filenames):
                fpath = os.path.join(dirpath, fname)
                rel_path = os.path.relpath(fpath, root)
                if is_production_file(rel_path):
                    cnt = count_substantive_lines(fpath)
                    file_counts[rel_path] = cnt
                    total_lines += cnt

    ledger = {
        "kernel_dirs": KERNEL_DIRS,
        "total_kernel_lines": total_lines,
        "files": file_counts,
        "minimum_target": 10000,
        "target_met": total_lines >= 10000,
    }

    os.makedirs(".agent-local", exist_ok=True)
    ledger_path = os.path.join(".agent-local", "scope_ledger.json")
    with open(ledger_path, "w", encoding="utf-8") as f:
        json.dump(ledger, f, indent=2)

    print(f"Total substantive kernel/driver lines: {total_lines}")
    print(f"Target: 10000 (status: {'MET' if ledger['target_met'] else 'IN PROGRESS'})")
    print(f"Ledger written to {ledger_path}")
    return 0 if ledger["target_met"] else 1


if __name__ == "__main__":
    sys.exit(main())
