#!/usr/bin/env python3
"""Compare equal native work on one host using two independently built kernels."""

import argparse
import json
import os
from pathlib import Path
import shutil
from statistics import median, quantiles
import subprocess
import tempfile

from evidence_format import file_hash
from test_guest_bench import SAMPLES, parse_samples
from test_tcp_campaign import verify_wire
from vm import firmware, tool


OVERLAY = ("tests/guest/bench_campaign.c", "scripts/test_guest_bench.py")
PROFILES = ("1", "4")


def git(root, *args):
    return subprocess.check_output(["git", "-C", str(root), *args], text=True, timeout=30).strip()


def execute(root, log, command, timeout):
    with log.open("ab") as stream:
        subprocess.run(command, cwd=root, stdout=stream, stderr=subprocess.STDOUT, timeout=timeout, check=True)


def host_identity():
    return {
        "system": os.uname().sysname,
        "machine": os.uname().machine,
        "qemu": subprocess.check_output(
            [tool("qemu-system-x86_64"), "--version"], text=True, timeout=10
        ).splitlines()[0],
        "firmware_sha256": file_hash(Path(firmware())),
        "profiles": {"cpus": [1, 4], "memory_mib": 256, "machine": "q35,accel=tcg"},
    }


def experiment(root, folder, revision, overlay):
    with tempfile.TemporaryDirectory(prefix="bench-source-", dir=root / "build") as work:
        source = Path(work) / "source"
        subprocess.run(["git", "clone", "--quiet", "--local", "--shared", "--no-checkout", str(root), str(source)],
                       timeout=60, check=True)
        subprocess.run(["git", "-C", str(source), "checkout", "--quiet", "--detach", revision], timeout=30, check=True)
        for file in overlay:
            shutil.copyfile(root / file, source / file)
        if set(git(source, "diff", "--name-only").splitlines()) != set(overlay):
            raise RuntimeError("benchmark overlay changed unexpected source files")
        log = folder / "console.log"
        folder.mkdir()
        execute(source, log, ("make", "-j4", "image"), 240)
        execute(source, log, ("python3", "scripts/test_guest_bench.py"), 600)
        shutil.copytree(source / "build/guest-bench", folder / "raw")
        return {
            "source": revision, "tree": git(source, "rev-parse", "HEAD^{tree}"),
            "kernel_sha256": file_hash(source / "build/stratum.elf"),
            "boot_sha256": file_hash(source / "build/images/stratum-boot.img"),
            "overlay": {file: file_hash(source / file) for file in overlay},
        }


def summary(samples):
    first, _, third = quantiles(samples, n=4, method="inclusive")
    return {"median": median(samples), "q1": first, "q3": third, "samples": len(samples)}


def extract(raw, identity, firmware_hash, workload_hash):
    report = json.loads((raw / "samples.json").read_text())
    if report["inputs"]["boot"] != identity["boot_sha256"] or report["inputs"]["firmware"] != firmware_hash or (
        report["inputs"]["workload"] != workload_hash or report["profiles"].keys() != set(PROFILES)
    ):
        raise ValueError("benchmark input or CPU identity mismatch")
    profiles = {}
    for cpu, profile in report["profiles"].items():
        guest = parse_samples((raw / f"{cpu}cpu-boot-31.log").read_bytes())
        if guest != profile["guest_tsc_ticks"]:
            raise ValueError("benchmark guest samples disagree with the transcript")
        result = {key: {"work": SAMPLES[key][1], "unit": "guest_tsc_ticks", **summary(values)}
                  for key, values in guest.items()}
        boot = profile["host_boot_ns"]
        tcp = profile["tcp"]
        capture = raw / f"{cpu}cpu-tcp.pcap"
        if len(boot) != 30 or len(tcp["samples_ns"]) != 10 or tcp["bytes_each"] != 262144 or any(
            type(value) is not int or value <= 0 for value in (*boot, *tcp["samples_ns"])
        ) or file_hash(capture) != tcp["pcap_sha256"] or (
            verify_wire(capture, 12, 131072) != tcp["wire_payload_sha256"]
        ):
            raise ValueError("benchmark host samples or TCP payload evidence mismatch")
        result["boot"] = {"work": 1, "unit": "host_ns", **summary(boot)}
        result["tcp"] = {"work": tcp["bytes_each"], "unit": "host_ns", **summary(tcp["samples_ns"])}
        profiles[cpu] = result
    return profiles, report["inputs"]["disk"]


def compare(folder, report, workload_hash):
    data = {}
    disks = set()
    for label in ("baseline", "candidate"):
        data[label], disk = extract(folder / label / "raw", report[label],
                                    report["host"]["firmware_sha256"], workload_hash)
        disks.add(disk)
    if len(disks) != 1:
        raise ValueError("benchmark disk templates differ")
    for cpu in PROFILES:
        for key, old in data["baseline"][cpu].items():
            new = data["candidate"][cpu][key]
            if any(old[field] != new[field] for field in ("work", "unit", "samples")):
                raise ValueError(f"unequal benchmark work on {cpu} CPU(s): {key}")
    return data


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("baseline")
    args = parser.parse_args()
    root = Path.cwd()
    if git(root, "status", "--porcelain"):
        raise RuntimeError("freeze source before benchmarking")
    baseline = git(root, "rev-parse", f"{args.baseline}^{{commit}}")
    candidate = git(root, "rev-parse", "HEAD")
    if baseline == candidate:
        raise RuntimeError("comparison requires distinct source revisions")
    folder = root / "build" / "benchmark-comparison"
    folder.mkdir(parents=True)
    before = host_identity()
    report = {
        "host": before,
        "baseline": experiment(root, folder / "baseline", baseline, OVERLAY),
        "candidate": experiment(root, folder / "candidate", candidate, ()),
    }
    if host_identity() != before:
        raise RuntimeError("benchmark environment changed during comparison")
    report["profiles"] = compare(folder, report, file_hash(root / OVERLAY[0]))
    (folder / "comparison.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print("[PASS] Matched completed work and recorded median and interquartile range on both profiles")


if __name__ == "__main__":
    main()
