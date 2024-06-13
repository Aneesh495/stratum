#!/usr/bin/env python3
"""Verify the volatile mount stays independent from StrataFS across reboot."""

from pathlib import Path

from test_smoke import PROMPT, require, shell
from vm import BOOT_IMAGE, command, fresh_disk, running


def main():
    logs = Path("build/tmpfs")
    with fresh_disk() as disk:
        cmd = command(BOOT_IMAGE, disk, cpus=4)
        with running(cmd, logs / "initial.log") as guest:
            require(b"at '/tmp' (type tmpfs)", guest.expect(PROMPT))
            require(b"[PASS] tmpfs mount, sparse I/O, namespace and retained handles",
                    shell(guest, "/bin/fs-campaign tmpfs"))
            require(b"volatile", shell(guest, "ls /tmp"))
        with running(cmd, logs / "reboot.log") as guest:
            guest.expect(PROMPT)
            require(b"err=-2", shell(guest, "stat /tmp/volatile"))
            if shell(guest, "cat /persistent") != b"disk":
                raise RuntimeError("StrataFS fixture changed across tmpfs reboot")
    print("[PASS] tmpfs contents cleared after reboot while StrataFS retained exact bytes")


if __name__ == "__main__":
    main()
