#!/usr/bin/env python3
"""Run concurrent userspace, disk and Ethernet workloads in one uninterrupted VM."""

import argparse
from hashlib import sha256
import json
from pathlib import Path
import re
import socket
import subprocess
import tempfile
import time

from evidence_format import file_hash
from test_net_wrap import WINDOW, arp_exchange, check_reply, expected_payload, frame, framed, recv_frame
from test_smoke import PROMPT, require, shell
from vm import BOOT_IMAGE, command, fresh_disk, running, tool


DURATION = 3600
DATAGRAMS = 128
WARMUP_ROUNDS = 5
PROGRESS = (b"[PASS] soak CPUs=0xff faults=256 processes=9 threads=32 "
            b"transfers=32 fsyncs=4 fds=61")


def observe(guest):
    reply = shell(guest, "sysinfo")
    uptime, cpus, tasks, total, free, calls = (
        int(re.search(label + rb":\s+(\d+)", reply)[1])
        for label in (b"Uptime", b"Online CPUs", b"Processes", b"Total Memory", b"Free Memory", b"Syscalls")
    )
    if cpus != 8 or not 900000 <= total <= 1048576:
        raise RuntimeError(f"wrong soak profile: {cpus} CPUs, {total} KiB")
    return {"uptime_ms": uptime, "processes": tasks, "free_kib": free, "syscalls": calls}


def exercise(guest, peer, reader, digest):
    guest.send(b"/bin/soak-campaign\r")
    guest.expect(f"net-campaign: ready for {DATAGRAMS} datagrams", timeout=30)
    for start in range(0, DATAGRAMS, WINDOW):
        batch = [expected_payload(i) for i in range(start, start + WINDOW)]
        peer.sendall(b"".join(framed(frame(data, i)) for i, data in enumerate(batch, start)))
        for data in batch:
            check_reply(recv_frame(reader), data)
            digest.update(data)
    reply = guest.expect(PROMPT, timeout=30)
    require(f"[PASS] UDP echoes={DATAGRAMS} exact payloads".encode(), reply)
    require(PROGRESS, reply)


def run(seconds, logs):
    checker = logs / "strata_check"
    subprocess.run([tool("clang"), "-O2", "tools/strata_check.c", "-o", str(checker)],
                   check=True, timeout=30)
    kernel = Path("build/stratum.elf")
    kernel_hash, boot_hash = file_hash(kernel), file_hash(BOOT_IMAGE)
    capture = logs / "ethernet.pcap"
    digest = sha256()
    with fresh_disk() as disk, socket.socket() as listener, (logs / "rounds.jsonl").open("w") as journal:
        listener.bind(("127.0.0.1", 0))
        listener.listen(1)
        listener.settimeout(20)
        port = listener.getsockname()[1]
        cmd = command(BOOT_IMAGE, disk, cpus=8, memory="1G",
                      ethernet=f"connect=127.0.0.1:{port}", capture=capture)
        with running(cmd, logs / "guest.log") as guest:
            peer, _ = listener.accept()
            with peer, peer.makefile("rb") as reader:
                peer.settimeout(10)
                peer.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                guest.expect(PROMPT)
                arp_exchange(peer, reader)
                for _ in range(WARMUP_ROUNDS):
                    exercise(guest, peer, reader, sha256())
                baseline = observe(guest)
                print(f"Soak baseline: {baseline}", flush=True)
                start = time.monotonic()
                last = baseline
                rounds = 0
                next_report = 60
                while True:
                    exercise(guest, peer, reader, digest)
                    current = observe(guest)
                    elapsed = time.monotonic() - start
                    rounds += 1
                    journal.write(json.dumps({"round": rounds, "elapsed_seconds": elapsed, **current}) + "\n")
                    journal.flush()
                    if (current["processes"], current["free_kib"]) != (baseline["processes"], baseline["free_kib"]):
                        raise RuntimeError(f"resources changed after round {rounds}: {baseline} to {current}")
                    if current["uptime_ms"] <= last["uptime_ms"] or current["syscalls"] <= last["syscalls"]:
                        raise RuntimeError(f"guest counters stopped after round {rounds}")
                    last = current
                    if elapsed >= seconds:
                        break
                    if elapsed >= next_report:
                        print(f"Soak running: {elapsed:.0f}s, {rounds} verified rounds", flush=True)
                        next_report += 60

        with (logs / "disk.log").open("w") as transcript:
            subprocess.run([str(checker), str(disk)], stdout=transcript, stderr=subprocess.STDOUT,
                           check=True, timeout=30)
        if (file_hash(kernel), file_hash(BOOT_IMAGE)) != (kernel_hash, boot_hash):
            raise RuntimeError("kernel or boot image changed during the soak")
        report = {
            "cpus": 8, "memory": "1G", "requested_seconds": seconds, "elapsed_seconds": elapsed,
            "rounds": rounds, "baseline": baseline, "final": last,
            "demand_faults": rounds * 256, "processes": rounds * 9, "thread_joins": rounds * 32,
            "ipc_transfers": rounds * 32, "disk_fsyncs": rounds * 4,
            "udp_datagrams_per_direction": rounds * DATAGRAMS, "udp_payload_sha256": digest.hexdigest(),
            "pcap_sha256": file_hash(capture), "disk_sha256": file_hash(disk),
            "boot_image_sha256": boot_hash, "kernel_sha256": kernel_hash,
        }
        (logs / "results.json").write_text(json.dumps(report, indent=2) + "\n")
    print(f"[PASS] {rounds} verified mixed rounds in {elapsed:.1f}s, quiescent resources reconciled", flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--seconds", type=int, default=DURATION)
    args = parser.parse_args()
    if not 1 <= args.seconds <= DURATION:
        parser.error(f"expected 1 to {DURATION} seconds")
    logs = Path(tempfile.mkdtemp(prefix="soak-", dir="build"))
    print(f"Writing soak evidence to {logs}", flush=True)
    run(args.seconds, logs)
