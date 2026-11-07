#!/usr/bin/env python3
"""Exercise targeted thread cancellation and process exit across blocking operations."""

from pathlib import Path

from test_smoke import PROMPT, require
from vm import BOOT_IMAGE, command, running


def main():
    for cpus in (4, 8):
        with running(command(BOOT_IMAGE, cpus=cpus), Path(f"build/cancellation/{cpus}cpu.log")) as guest:
            guest.expect(PROMPT)
            guest.send(b"/bin/cancel-campaign\r")
            raw = guest.expect(PROMPT, timeout=420)
            require(b"Socket poll races=2500 and orphan process reaps=32", raw)
            require((f"[PASS] cancellations=10008 group_exits=256 CPUs={cpus} "
                     "with pages, threads, and files reconciled").encode(), raw)
        print(f"[PASS] {cpus} CPUs: 10008 targeted cancellations and 256 process exits", flush=True)


if __name__ == "__main__":
    main()
