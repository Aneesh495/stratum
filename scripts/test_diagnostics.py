#!/usr/bin/env python3
"""Run opt-in kernel checks with bounded waits and disposable disk images."""

import argparse
import json
from pathlib import Path
import subprocess
import sys
import tempfile

from vm import BOOT_IMAGE, boot_profile, command, fresh_disk, running


MARKERS = {
    "mm": "P04 memory verification tests passed",
    "smp": "P05 SMP synchronization verification passed",
    "sched": "P06 scheduler verification passed",
    "abi": "P08 Processes, Threads, and IPC gates verified",
    "drivers": "Virtio-blk synchronous read, write, flush, and queue index cycling verified.",
    "storage": "P10 & P11 VFS and StrataFS storage verified",
    "stress": "Guest stress smoke completed",
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("suite", choices=[*MARKERS, "faults", "all"])
    args = parser.parse_args()
    suites = ["stress", "faults"] if args.suite == "all" else [args.suite]
    logs = Path("build/diagnostics")
    for suite in suites:
        fault = suite == "faults"
        profiles = (2,) if fault else (1, 2, 4, 8)
        for cpus in profiles:
            with tempfile.TemporaryDirectory(dir="build") as directory, fresh_disk() as disk:
                boot = boot_profile(BOOT_IMAGE, Path(directory), "fault=ud2" if fault else "selftest")
                cmd = command(boot, disk, cpus=cpus, memory="1G" if suite == "stress" and cpus == 8 else "256M")
                with running(cmd, logs / f"{suite}-{cpus}.log") as guest:
                    if fault:
                        raw = guest.expect("System halted.", allow_panic=True)
                        if b"INTERRUPT FRAME DUMP" not in raw or b"Invalid Opcode (#UD)" not in raw:
                            raise RuntimeError("expected decoded #UD trap and halt")
                    else:
                        raw = guest.expect(MARKERS[suite])
                        if f"{cpus} CPU(s) online and active".encode() not in raw and cpus != 1:
                            raise RuntimeError(f"expected {cpus} online CPUs")
                if fault:
                    symbols = subprocess.check_output([
                        sys.executable, "tools/symbolize_panic.py", "build/stratum.elf", str(logs / f"{suite}-{cpus}.log")
                    ], timeout=15)
                    frames = json.loads(symbols)["frames"]
                    if len(frames) != 1 or frames[0]["symbol"] != "kmain" or frames[0]["opcode"] != "0f0b":
                        raise RuntimeError("kernel fault did not resolve to the injected ud2")
                    (logs / f"{suite}-{cpus}-symbols.json").write_bytes(symbols)
                print(f"[PASS] {suite}, {cpus} vCPU(s)", flush=True)


if __name__ == "__main__":
    main()
