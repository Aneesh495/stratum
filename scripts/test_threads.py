#!/usr/bin/env python3
"""Check native user threads, TLS and SIMD state in the normal guest."""

from collections import Counter
from pathlib import Path
import re

from test_smoke import PROMPT, require
from vm import BOOT_IMAGE, command, running


def check_population(output, cpus):
    live = re.findall(rb"^\[LIVE\] pid=(\d+) worker=(\d+) tid=(\d+) cpu=(\d+)\r?$", output, re.M)
    pids = Counter(pid for pid, _, _, _ in live)
    if len(live) != 512 or len(pids) != 32 or set(pids.values()) != {16}:
        raise RuntimeError(f"wrong live population: {len(live)} workers across {len(pids)} processes")
    if len({tid for _, _, tid, _ in live}) != len(live) or len({(pid, worker) for pid, worker, _, _ in live}) != len(live):
        raise RuntimeError("duplicate live thread identity")
    if {int(cpu) for _, _, _, cpu in live} != set(range(cpus)):
        raise RuntimeError("not all CPUs ran the workload")
    require(f"[BARRIER] generation=G live=512 processes=32 cpu_mask={(1 << cpus) - 1:x}".encode(), output)
    counts = re.findall(rb"^\[SWITCH\] cpu=(\d+) count=(\d+)\r?$", output, re.M)
    total = sum(int(count) for _, count in counts)
    if [int(cpu) for cpu, _ in counts] != list(range(cpus)) or any(int(count) <= 0 for _, count in counts) or total < 1000000:
        raise RuntimeError(f"wrong switch counts: {counts!r}")
    require(f"[PASS] distinct runnable user switches={total} completed workers=512".encode(), output)
    return total


def check(cpus):
    with running(command(BOOT_IMAGE, cpus=cpus, memory="1G"), Path(f"build/threads/{cpus}cpu.log")) as guest:
        guest.expect(PROMPT)
        guest.send(b"/bin/thread-campaign boundaries\r")
        require(b"[PASS] syscall boundaries=10000 across ten families", guest.expect(PROMPT, timeout=60))
        guest.send(b"/bin/thread-campaign lifecycle\r")
        result = guest.expect(PROMPT, timeout=360)
        require(b"[PASS] thread create/join cycles=25000 with FS TLS, XMM state, stale TIDs and resource recovery", result)
        print(f"[PASS] {cpus} CPUs: 25000 joins with TLS, SIMD and resource recovery", flush=True)
        guest.send(b"/bin/thread-campaign population\r")
        result = guest.expect(PROMPT, timeout=180)
        switches = check_population(result, cpus)
        require(b"Every runnable worker completed within 60000 guest milliseconds", result)
        guest.send(b"/bin/thread-campaign futex\r")
        require(b"[PASS] futex cases=10000 mismatch=2500 wake=2500 timeout=2500 unmap=2500",
                guest.expect(PROMPT, timeout=180))
    print(f"[PASS] {cpus} CPUs: 512 simultaneous workers, {switches} real user switches, 10000 futex cases", flush=True)


if __name__ == "__main__":
    for cpus in (4, 8):
        check(cpus)
