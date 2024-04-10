#!/usr/bin/env python3
"""Kill QEMU at ten journal stages and check the allowed file state after reboot."""

import argparse
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import time

from evidence_format import file_hash
from test_smoke import PROMPT, shell
from vm import BOOT_IMAGE, boot_profile, command, fresh_disk, running, tool


STAGES = (
    ("before-descriptor", False),
    ("after-descriptor", False),
    ("first-payload", False),
    ("payload-flushed", False),
    ("commit-written", False),
    ("commit-flushed", False),
    ("published", True),
    ("first-home", True),
    ("home-flushed", True),
    ("checkpointed", True),
)


def run_case(profile, baseline, label, published, repetition, destination, checker):
    case = f"{repetition:02d}-{label}"
    case_dir = destination / case
    case_dir.mkdir()
    start = time.monotonic()
    with tempfile.TemporaryDirectory(dir="build") as directory:
        disk = Path(directory) / "recovery.img"
        shutil.copyfile(baseline, disk)
        before = file_hash(disk)
        new_payload = f"run-{repetition:02d}-{label}"
        with running(command(profile, disk, cpus=1), case_dir / "interrupted.log") as guest:
            guest.expect(PROMPT)
            guest.send(f"write /crash-record {new_payload}\r".encode())
            guest.expect(f"WAL crash stage {label}.")
            guest.interrupt()
        after_stop = file_hash(disk)

        with running(command(BOOT_IMAGE, disk, cpus=1), case_dir / "reboot.log") as guest:
            transcript = guest.expect(PROMPT)
            replay_expected = published and label != "checkpointed"
            if (b"Replayed committed transaction" in transcript) != replay_expected:
                raise RuntimeError(f"{case}: unexpected replay outcome")
            expected = b"baseline\r\n"
            if published:
                expected += new_payload.encode() + b"\r\n"
            observed = shell(guest, "cat /crash-record")
            if expected != observed:
                raise RuntimeError(f"{case}: expected {expected!r}, got {observed!r}")

        with (case_dir / "checker.log").open("w") as log:
            subprocess.run([checker, str(disk)], check=True, timeout=10,
                           stdout=log, stderr=subprocess.STDOUT)
        report = {
            "stage": label,
            "repetition": repetition,
            "before": before,
            "after_interruption": after_stop,
            "after_reboot": file_hash(disk),
            "replayed": replay_expected,
            "payload": observed.decode("ascii").splitlines(),
            "seconds": round(time.monotonic() - start, 3),
        }
        (case_dir / "result.json").write_text(json.dumps(report, indent=2) + "\n")
    print(f"[PASS] {case}: {'new record' if published else 'old record'}", flush=True)
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repeats", type=int, choices=range(1, 11), default=1)
    args = parser.parse_args()
    destination = Path(tempfile.mkdtemp(prefix="wal-crash-", dir="build"))
    checker = "build/strata_check"
    subprocess.run([tool("clang"), "-Wall", "-Wextra", "-O2",
                    "tools/strata_check.c", "-o", checker], check=True, timeout=30)
    print(f"Writing crash campaign to {destination}", flush=True)
    rows = []
    with fresh_disk() as baseline, tempfile.TemporaryDirectory(dir="build") as directory:
        with running(command(BOOT_IMAGE, baseline, cpus=1), destination / "setup.log") as guest:
            guest.expect(PROMPT)
            if shell(guest, "write /crash-record baseline"):
                raise RuntimeError("baseline write failed")
        for label, published in STAGES:
            stage_dir = Path(directory) / label
            stage_dir.mkdir()
            profile = boot_profile(BOOT_IMAGE, stage_dir, "crash=wal-" + label)
            for repeat in range(1, args.repeats + 1):
                rows.append(run_case(profile, baseline, label, published, repeat, destination, checker))
    report = {
        "kernel_sha256": file_hash(Path("build/stratum.elf")),
        "boot_image_sha256": file_hash(BOOT_IMAGE),
        "completed": len(rows),
        "stages": len(STAGES),
        "cases": rows,
    }
    (destination / "campaign.json").write_text(json.dumps(report, indent=2) + "\n")
    print(f"[PASS] {len(rows)} interruptions verified across {len(STAGES)} WAL stages.", flush=True)


if __name__ == "__main__":
    main()
