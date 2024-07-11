#!/usr/bin/env python3
"""Observe init reaping and restarting its shell on the same boot."""

from pathlib import Path
import re

from test_smoke import PROMPT, require, shell
from vm import BOOT_IMAGE, command, fresh_disk, running


def shell_pid(guest):
    listing = shell(guest, "ps")
    require(b"PID PPID STATE NAME\r\n1 0 alive init\r\n", listing)
    rows = re.findall(rb"^(\d+) 1 alive sh\r?$", listing, re.M)
    if len(rows) != 1 or b"zombie" in listing:
        raise RuntimeError(f"unexpected shell process listing: {listing!r}")
    return int(rows[0])


def main():
    for cpus, memory in ((1, "64M"), (4, "256M")):
        log = Path(f"build/supervision/{cpus}cpu.log")
        with fresh_disk() as disk:
            with running(command(BOOT_IMAGE, disk, cpus=cpus, memory=memory), log) as guest:
                guest.expect(PROMPT)
                before = shell_pid(guest)
                if shell(guest, "write /supervision same-boot"):
                    raise RuntimeError("could not create supervision fixture")
                for _ in range(4):
                    require(f"[INIT] Reaped shell pid={before} status=0".encode(), shell(guest, "exit"))
                    after = shell_pid(guest)
                    if after <= before:
                        raise RuntimeError("supervisor reused or retained a dead shell")
                    before = after
                if shell(guest, "cat /supervision") != b"same-boot\r\n":
                    raise RuntimeError("supervision fixture changed across restart")
        if log.read_bytes().count(b"[KERNEL] Kernel entered at higher-half entry point") != 1:
            raise RuntimeError("unexpected reboot during supervision")
        print(f"[PASS] {cpus} CPUs and {memory}: PID 1 reaped and restarted the shell four times")


if __name__ == "__main__":
    main()
