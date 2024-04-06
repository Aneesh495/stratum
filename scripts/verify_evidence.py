#!/usr/bin/env python3
"""Verify a committed-source regression bundle without running or rewriting it."""

import argparse
from datetime import datetime
import json
from pathlib import Path
import re
import subprocess
import sys

from evidence_format import ARTIFACTS, COMMANDS, PROFILES, REQUIRED_TRANSCRIPTS, SCHEMA, file_hash
from test_replication import check_capture


def check(condition, message):
    if not condition:
        raise ValueError(message)


def git(*args):
    return subprocess.check_output(["git", *args], text=True, timeout=30).strip()


def verify(root):
    data = json.loads((root / "manifest.json").read_text(encoding="utf-8"))
    check(data["schema"] == SCHEMA and data["kind"] == "regression", "unsupported schema or bundle kind")
    check(data["full_acceptance"] == "unverified", "regression bundle cannot certify full acceptance")
    check(datetime.fromisoformat(data["created_at"]).utcoffset() is not None, "missing timestamp offset")
    check(data["host"]["system"] and data["host"]["machine"], "missing host identity")
    check(not git("status", "--porcelain"), "current source tree is dirty")
    check(data["source_commit"] == git("rev-parse", "HEAD"), "source commit does not match checkout")
    check(data["source_tree"] == git("rev-parse", "HEAD^{tree}"), "source tree does not match checkout")

    results = data["commands"]
    check(results.keys() == COMMANDS.keys(), "missing or unexpected workload")
    for name, expected in COMMANDS.items():
        row = results[name]
        check(row["command"] == list(expected), f"{name}: wrong command")
        check(type(row["exit"]) is int and row["exit"] == 0, f"{name}: unsuccessful exit")
        check(isinstance(row["seconds"], (int, float)) and 0 < row["seconds"] < 600, f"{name}: invalid duration")

    paths = (
        {f"logs/{name}.log" for name in COMMANDS}
        | {f"artifacts/{label}/{name}" for label in ("build-a", "build-b") for name in ARTIFACTS}
        | {f"transcripts/{name}" for name in REQUIRED_TRANSCRIPTS}
    )
    check(data["files"].keys() == paths, "missing or unexpected artifact")
    for name in sorted(paths):
        path = root / name
        check(path.is_file() and path.resolve().is_relative_to(root.resolve()) and not path.is_symlink()
              and path.stat().st_size > 0, f"{name}: missing, empty, or unsafe path")
        check(file_hash(path) == data["files"][name], f"{name}: content digest mismatch")

    for name in ARTIFACTS:
        check(data["files"][f"artifacts/build-a/{name}"] == data["files"][f"artifacts/build-b/{name}"],
              f"{name}: independent build outputs differ")

    def read(name):
        return (root / name).read_text(encoding="utf-8", errors="replace")

    check(re.search(r"\(SHA-256 [0-9a-f]{64}\)", read("logs/doctor.log")), "firmware digest missing")
    host = read("logs/host.log")
    check("2000000 / 2000000 ops completed" in host and
          "All deterministic models completed successfully" in host and
          host.count("Completed: 350000 iterations") == 6, "host operation counts missing")
    check(read("logs/boot.log").count("[PASS] rejected ") == 17, "loader rejection count mismatch")
    for cpus, memory in PROFILES:
        name = f"{cpus}cpu-{memory}"
        boot = read(f"transcripts/smoke/{name}-boot.log")
        reboot = read(f"transcripts/smoke/{name}-reboot.log")
        check(f"Online CPUs:  {cpus}\n" in boot and "durable-alpha\ndurable-beta\n" in boot and
              "durable-alpha\ndurable-beta\n" in reboot and "err=-2" in reboot, f"{name}: shell or recovery mismatch")
    for cpus in (1, 2, 4, 8):
        log = read(f"transcripts/diagnostics/stress-{cpus}.log")
        check("64 fork/reap cycles" in log and "128 demand-zero pages" in log and
              "Guest stress smoke completed" in log,
              f"{cpus} CPUs: missing ABI or stress observation")
    check("Invalid Opcode (#UD)" in read("transcripts/diagnostics/faults-2.log"), "missing kernel fault diagnostic")
    check("WAL published before home writes." in read("transcripts/recovery/interrupted.log") and
          "Replayed committed transaction" in read("transcripts/recovery/replayed.log") and
          "Journal initialization failed: -5" in read("transcripts/recovery/rejected.log"), "WAL recovery evidence mismatch")
    check("duplicate/conflict handling" in read("logs/ledger.log"), "missing TCP ledger assertion results")
    check("via-ethernet" in read("transcripts/replication/follower-reboot.log"), "missing follower recovery payload")
    check_capture(root / "transcripts/replication/ethernet.pcap", b"GET peer-1\n", b"OK via-ethernet\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("bundle", nargs="?", type=Path)
    args = parser.parse_args()
    root = args.bundle
    if root is None:
        candidates = sorted(path for path in Path("build/evidence").glob("20*") if path.is_dir())
        if not candidates:
            print("No regression bundle found. Run make evidence.", file=sys.stderr)
            return 1
        root = candidates[-1]
    try:
        verify(root)
    except (OSError, ValueError, KeyError, TypeError, RuntimeError, subprocess.SubprocessError) as error:
        print(f"Evidence rejected: {error}", file=sys.stderr)
        return 1
    print(f"Verified regression bundle {root}. Full acceptance remains unverified.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
