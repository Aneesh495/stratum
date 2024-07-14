#!/usr/bin/env python3
"""Enter from ring 3 and exercise two nested IST stack switches on each CPU."""

from pathlib import Path
import re
import tempfile

from test_smoke import PROMPT, require, shell
from vm import BOOT_IMAGE, boot_profile, command, running


def check_stacks(raw, cpus):
    rows = re.findall(rb"^\[IST\] cpu=(\d+) entry=([0-9a-f]+) ist1=([0-9a-f]+) ist2=([0-9a-f]+)\r?$", raw, re.M)
    if [int(row[0]) for row in rows] != list(range(cpus)):
        raise RuntimeError("missing nested interrupt observation")
    stacks = [tuple(int(address, 16) for address in row[1:]) for row in rows]
    private = {address for _, first, second in stacks for address in (first, second)}
    if len(private) != 2 * cpus or any(entry in private or min(entry, first, second) < 0xffff800000000000
                                      for entry, first, second in stacks):
        raise RuntimeError("nested interrupt stacks overlap or are outside the kernel")
    require(f"[PASS] nested protected stacks on {cpus} CPUs".encode(), raw)


def main():
    with tempfile.TemporaryDirectory(dir="build") as work:
        boot = boot_profile(BOOT_IMAGE, Path(work), "irq=stacks")
        for cpus in (1, 4, 8):
            with running(command(boot, cpus=cpus), Path(f"build/entry-stacks/{cpus}cpu.log")) as guest:
                guest.expect(PROMPT)
                check_stacks(shell(guest, "/bin/thread-campaign entry"), cpus)
    print("[PASS] ring 3 entry and two nested IST switches on 1, 4 and 8 vCPUs")


if __name__ == "__main__":
    main()
