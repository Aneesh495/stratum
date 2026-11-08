#!/usr/bin/env python3
"""Exercise monotonic sleep deadlines while eligible native tasks change CPU."""

from pathlib import Path
import re

from test_smoke import PROMPT, require
from vm import BOOT_IMAGE, command, running


def check_deadlines(raw, cpus):
    rows = re.findall(rb"^\[DEADLINE\] pid=(\d+) migrations=(\d+) cpu_mask=([0-9a-f]+)\r?$", raw, re.M)
    if len(rows) != 32 or len({row[0] for row in rows}) != 32:
        raise RuntimeError("missing unique native deadline workers")
    moves, mask = 0, 0
    for _, migrations, cpumask in rows:
        count, observed = int(migrations), int(cpumask, 16)
        if not observed or observed >= 1 << cpus or count > 128 or (count and observed.bit_count() < 2):
            raise RuntimeError("invalid migration observation")
        moves += count
        mask |= observed
    if not moves or mask != (1 << cpus) - 1:
        raise RuntimeError("no migration or missing CPU participation")
    require(f"[PASS] deadlines=4096 migrations={moves} CPU mask={mask:x}".encode(), raw)


def main():
    for cpus in (4, 8):
        with running(command(BOOT_IMAGE, cpus=cpus), Path(f"build/deadlines/{cpus}cpu.log")) as guest:
            guest.expect(PROMPT)
            guest.send(b"/bin/thread-campaign deadlines\r")
            check_deadlines(guest.expect(PROMPT, timeout=45), cpus)
    print("[PASS] 4096 native deadlines with actual migrations on each CPU profile")


if __name__ == "__main__":
    main()
