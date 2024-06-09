#!/usr/bin/env python3
"""Check real inherited shared pages and private COW on two SMP profiles."""

from pathlib import Path

from test_smoke import PROMPT
from vm import BOOT_IMAGE, command, running


def main():
    with running(command(BOOT_IMAGE, cpus=1, memory="64M"), Path("build/shared/exhaust.log")) as guest:
        guest.expect(PROMPT)
        guest.send(b"/bin/shared-campaign exhaust\r")
        guest.expect("[PASS] allocator exhaustion, rollback, retry and final page recovery", timeout=60)
        guest.expect(PROMPT)
    print("[PASS] 64 MiB: real shared allocation exhaustion and rollback", flush=True)
    for cpus in (4, 8):
        with running(command(BOOT_IMAGE, cpus=cpus), Path(f"build/shared/{cpus}cpu.log")) as guest:
            guest.expect(PROMPT)
            guest.send(b"/bin/shared-campaign\r")
            guest.expect("[PASS] shared mappings=2000 COW=2000 final-owner handoff, protection and exact page recovery",
                         timeout=120)
            guest.expect(PROMPT)
        print(f"[PASS] {cpus} CPUs: inherited shared mappings, private COW and last-owner lifetime", flush=True)


if __name__ == "__main__":
    main()
