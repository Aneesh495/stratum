#!/usr/bin/env python3
"""Collect native benchmark samples and keep guest ticks distinct from host time."""

from hashlib import sha256
import json
from pathlib import Path
import re
import socket
from statistics import median
import time

from evidence_format import file_hash
from test_smoke import PROMPT, require
from test_tcp_campaign import reference_client, server_completed, start_server, verify_wire
from vm import BOOT_IMAGE, DATA_IMAGE, command, firmware, fresh_disk, running


SAMPLES = {
    "timer": (30, 1000), "getpid": (30, 10000), "demand_zero": (30, 256),
    "pipe_roundtrip": (30, 2000), "block_write": (10, 131072),
    "fsync": (10, 1), "block_read": (10, 131072),
}


def parse_samples(raw):
    samples = {name: [] for name in SAMPLES}
    for kind, index, work, ticks in re.findall(rb"^\[BENCH\] (\w+) run=(\d+) work=(\d+) ticks=(\d+)\r?$", raw, re.M):
        name = kind.decode()
        count, expected = SAMPLES[name]
        rows = samples[name]
        if int(index) != len(rows) or len(rows) >= count or int(work) != expected or int(ticks) <= 0:
            raise RuntimeError(f"{name}: unexpected benchmark sample")
        rows.append(int(ticks))
    if any(len(samples[name]) != count for name, (count, _) in SAMPLES.items()):
        raise RuntimeError("benchmark samples missing")
    require(b"[PASS] Guest benchmark samples verified", raw)
    return samples


def record_storage_and_ipc(cpus, logs):
    boot_ns = []
    for run in range(32):
        with fresh_disk() as disk:
            start = time.monotonic_ns()
            with running(command(BOOT_IMAGE, disk, cpus=cpus), logs / f"{cpus}cpu-boot-{run:02d}.log") as guest:
                guest.expect(PROMPT)
                elapsed = time.monotonic_ns() - start
                if run >= 2:
                    boot_ns.append(elapsed)
                if run == 31:
                    guest.send(b"/bin/bench-campaign\r")
                    samples = parse_samples(guest.expect(PROMPT, timeout=120))
    return {"guest_tsc_ticks": samples, "host_boot_ns": boot_ns}


def record_tcp(cpus, logs):
    with socket.socket() as reservation:
        reservation.bind(("127.0.0.1", 0))
        port = reservation.getsockname()[1]
    capture = logs / f"{cpus}cpu-tcp.pcap"
    sessions, size = 12, 131072
    samples = []
    with running(command(BOOT_IMAGE, cpus=cpus, host_port=port, capture=capture),
                 logs / f"{cpus}cpu-tcp.log") as guest:
        start_server(guest, sessions)
        for i in range(sessions):
            start = time.monotonic_ns()
            reference_client(port, i, size)
            elapsed = time.monotonic_ns() - start
            if i >= 2:
                samples.append(elapsed)
        server_completed(guest, sessions, size)
    return {"samples_ns": samples, "bytes_each": size * 2, "wire_payload_sha256": verify_wire(capture, sessions, size),
            "pcap_sha256": file_hash(capture)}


def main():
    logs = Path("build/guest-bench")
    logs.mkdir(parents=True, exist_ok=True)
    report = {
        "inputs": {"boot": file_hash(BOOT_IMAGE), "disk": file_hash(DATA_IMAGE),
                   "firmware": file_hash(Path(firmware())),
                   "workload": sha256(Path("tests/guest/bench_campaign.c").read_bytes()).hexdigest()},
        "profiles": {},
    }
    for cpus in (1, 4):
        results = record_storage_and_ipc(cpus, logs)
        results["tcp"] = record_tcp(cpus, logs)
        report["profiles"][str(cpus)] = results
        print(f"[PASS] {cpus} CPUs: 30 boot and short samples, 10 long samples per workload", flush=True)
        for name, samples in results["guest_tsc_ticks"].items():
            print(f"{name}: median {median(samples)} guest TSC ticks per {SAMPLES[name][1]} units", flush=True)
        print(f"boot: median {median(results['host_boot_ns'])} host ns", flush=True)
        print(f"TCP: median {median(results['tcp']['samples_ns'])} host ns per {results['tcp']['bytes_each']} bytes",
              flush=True)
    if (file_hash(BOOT_IMAGE), file_hash(DATA_IMAGE)) != (report["inputs"]["boot"], report["inputs"]["disk"]):
        raise RuntimeError("benchmark image changed during measurement")
    (logs / "samples.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
