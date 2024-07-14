#!/usr/bin/env python3
"""Exercise targeted thread cancellation and process exit across blocking operations."""

from pathlib import Path
import re

from test_smoke import PROMPT, require
from vm import BOOT_IMAGE, command, running


def check_accounting(raw):
    observed = re.search(rb"\[ACCOUNTING\] free_before=(\d+) free_after=(\d+) slab_before=(\d+) "
                         rb"slab_after=(\d+) objects_before=(\d+) objects_after=(\d+)", raw)
    if not observed:
        raise RuntimeError("cancellation resource accounting missing")
    free_before, free_after, slab_before, slab_after, objects_before, objects_after = map(int, observed.groups())
    if free_before + slab_before * 4096 != free_after + slab_after * 4096 or objects_before != objects_after:
        raise RuntimeError("physical pages or live slab objects did not reconcile")


def main():
    for cpus in (4, 8):
        with running(command(BOOT_IMAGE, cpus=cpus), Path(f"build/cancellation/{cpus}cpu.log")) as guest:
            guest.expect(PROMPT)
            guest.send(b"/bin/cancel-campaign\r")
            raw = guest.expect(PROMPT, timeout=420)
            check_accounting(raw)
            require(b"Socket poll races=2500 and orphan process reaps=32", raw)
            require((f"[PASS] cancellations=10008 group_exits=256 CPUs={cpus} "
                     "with pages, threads, and files reconciled").encode(), raw)
        print(f"[PASS] {cpus} CPUs: 10008 targeted cancellations and 256 process exits", flush=True)


if __name__ == "__main__":
    main()
