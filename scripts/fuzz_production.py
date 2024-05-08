#!/usr/bin/env python3
"""Run sanitizer-enabled libFuzzer on the six production parser families."""

import argparse
from concurrent.futures import ThreadPoolExecutor
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import time

from build_cpio import make_entry
from evidence_format import file_hash
from test_net_wrap import checksum
from vm import tool


TARGETS = {
    "elf": ("boot/uefi/elf.c",),
    "handoff": ("kernel/core/boot_check.c",),
    "cpio": ("kernel/fs/cpio.c",),
    "acpi": ("kernel/drivers/madt.c",),
    "stratafs": (),
    "ipv4": ("kernel/net/ipv4_decode.c", "kernel/net/checksum.c"),
}
HARNESS = "tests/fuzz/production.c"
FULL_DURATION = 600
KERNEL_BASE = 0xFFFFFFFF80000000


def fixtures():
    ident = b"\x7fELF\x02\x01\x01" + bytes(9)
    elf = struct.pack("<16sHHIQQQIHHHHHH", ident, 2, 62, 1, KERNEL_BASE, 64, 0, 0, 64, 56, 1, 0, 0, 0)
    elf += struct.pack("<IIQQQQQQ", 1, 5, 120, KERNEL_BASE, 0, 16, 4096, 1) + b"\x90" * 16

    handoff = bytearray(368)
    struct.pack_into("<QII", handoff, 0, 0x5354524154554D31, 1, 368)
    struct.pack_into("<QIIQQQQQQ", handoff, 48, 0x9000, 2, 32, 0x200000, 0x100000,
                     KERNEL_BASE, KERNEL_BASE, 0x400000, 4096)
    handoff.extend(struct.pack("<IIQQQ", 1, 0, 0x500000, 0, 3))
    handoff.extend(struct.pack("<IIQQQ", 1, 0, 0x600000, 0, 7))

    cpio = make_entry(".", mode=0o40755) + make_entry("bin", mode=0o40755)
    cpio += make_entry("bin/one", b"native") + make_entry("bin/two", b"sample")
    cpio += make_entry("TRAILER!!!", mode=0, ino=0)

    acpi_records = struct.pack("<BBBBI", 0, 8, 0, 0, 1)
    acpi_records += struct.pack("<BBBBII", 1, 12, 0, 0, 0xfec00000, 0)
    acpi_records += struct.pack("<BBBBIH", 2, 10, 0, 0, 2, 0)
    acpi_records += struct.pack("<BBHQ", 5, 12, 0, 0xfee00000)
    madt = struct.pack("<4sI28xII", b"APIC", 44 + len(acpi_records), 0xfee00000, 1) + acpi_records
    dirent = struct.pack("<IHBB248s", 2, 256, 4, 1, b"test")
    directory = struct.pack("<IHBB248s", 1, 256, 1, 2, b".")
    packet = bytearray(struct.pack("!BBHHHBBHII", 0x45, 0, 24, 1, 0x4000, 64, 17, 0, 0x0a00020f, 0x0a000210))
    struct.pack_into("!H", packet, 10, checksum(bytes(packet)))
    return {
        "elf": (elf, elf[:-4]),
        "handoff": (handoff, handoff[:-1]),
        "cpio": (cpio, cpio[:-5]),
        "acpi": (madt, madt[:-3]),
        "stratafs": (dirent, directory + bytes(256)),
        "ipv4": (b"\0" + packet + b"data", b"\0" + packet[:15]),
    }


def git(*args):
    return subprocess.check_output(["git", *args], text=True, timeout=30).strip()


def compiler():
    mac_llvm = Path("/opt/homebrew/opt/llvm/bin/clang")
    return os.environ.get("CLANG") or (str(mac_llvm) if mac_llvm.is_file() else tool("clang"))


def run_target(name, seeds, build, clang, seconds):
    root = build / name
    initial, corpus = root / "seeds", root / "corpus"
    initial.mkdir(parents=True)
    corpus.mkdir()
    for i, seed in enumerate(seeds):
        (initial / str(i)).write_bytes(seed)
        shutil.copyfile(initial / str(i), corpus / str(i))
    binary = root / "fuzz"
    instrumentation = ["-fsanitize=fuzzer,address,undefined", "-fno-sanitize-recover=all"]
    compile_command = [clang, "-g", "-O1", "-Wall", "-Wextra", "-std=c17",
                       "-Iinclude", "-Iboot/uefi", f"-DFUZZ_{name.upper()}", *instrumentation,
                       HARNESS, *TARGETS[name], "-o", str(binary)]
    with (root / "compile.log").open("w") as log:
        subprocess.run(compile_command, check=True, timeout=120, stdout=log, stderr=subprocess.STDOUT)

    env = dict(os.environ)
    env["ASAN_OPTIONS"] = "abort_on_error=1:detect_leaks=" + ("0" if sys.platform == "darwin" else "1")
    env["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
    command = [str(binary), str(corpus), f"-max_total_time={seconds}", "-max_len=4096",
               "-timeout=5", "-rss_limit_mb=768", "-seed=20260426", "-print_final_stats=1",
               f"-artifact_prefix={root}/"]
    start = time.monotonic()
    logpath = root / "run.log"
    with logpath.open("w") as log:
        result = subprocess.run(command, env=env, timeout=seconds + 90, stdout=log, stderr=subprocess.STDOUT)
    elapsed = time.monotonic() - start
    transcript = logpath.read_text(errors="replace")
    count = re.search(r"stat::number_of_executed_units:\s+(\d+)", transcript)
    features = re.findall(r"cov:\s+(\d+)\s+ft:\s+(\d+)", transcript)
    if result.returncode or not count or not features or elapsed < seconds:
        raise RuntimeError(f"{name} failed. See {logpath}")
    stats = {
        "executions": int(count[1]), "elapsed_seconds": round(elapsed, 3),
        "covered_edges": int(features[-1][0]), "features": int(features[-1][1]),
        "corpus_files": len(list(corpus.iterdir())),
        "seed_sha256": [file_hash(path) for path in sorted(initial.iterdir())],
        "log_sha256": file_hash(logpath), "instrumentation": instrumentation,
        "asan_options": env["ASAN_OPTIONS"], "ubsan_options": env["UBSAN_OPTIONS"],
    }
    print(f"[PASS] {name}: {stats['executions']} inputs in {elapsed:.1f}s, {stats['covered_edges']} edges", flush=True)
    return stats


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--seconds", type=int, default=FULL_DURATION)
    parser.add_argument("--jobs", type=int, default=6)
    args = parser.parse_args()
    if not 1 <= args.jobs <= 6 or not 1 <= args.seconds <= FULL_DURATION:
        parser.error("expected 1 to 6 jobs and 1 to 600 seconds")
    source, tree = git("rev-parse", "HEAD"), git("rev-parse", "HEAD^{tree}")
    dirty = bool(git("status", "--porcelain"))
    if args.seconds == FULL_DURATION and dirty:
        parser.error("commit the source before running the full campaign")

    clang = compiler()
    version = subprocess.check_output([clang, "--version"], text=True, timeout=10).splitlines()[0]
    build = Path("build")
    build.mkdir(exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix="production-fuzz-", dir=build))
    print(f"Writing production fuzz evidence to {root}", flush=True)
    seeds = fixtures()
    with ThreadPoolExecutor(args.jobs) as pool:
        futures = {name: pool.submit(run_target, name, seeds[name], root, clang, args.seconds) for name in TARGETS}
        rows = {name: future.result() for name, future in futures.items()}

    total = sum(row["executions"] for row in rows.values())
    if args.seconds == FULL_DURATION and total < 2000000:
        raise RuntimeError(f"only {total} completed executions")
    if args.seconds == FULL_DURATION and (git("rev-parse", "HEAD") != source or git("status", "--porcelain")):
        raise RuntimeError("source changed during the fuzz campaign")
    report = {
        "source": source, "tree": tree, "dirty": dirty, "compiler": version,
        "seconds_per_target": args.seconds, "total_executions": total, "targets": rows,
    }
    (root / "results.json").write_text(json.dumps(report, indent=2) + "\n")
    print(f"[PASS] {total} production parser mutations across {len(rows)} targets", flush=True)


if __name__ == "__main__":
    main()
