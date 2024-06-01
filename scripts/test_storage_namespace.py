#!/usr/bin/env python3
"""Exercise real directory transactions and hard links across two guest boots."""

from pathlib import Path
import subprocess

from test_smoke import PROMPT, require, shell
from vm import BOOT_IMAGE, command, fresh_disk, running, tool


def main():
    logs = Path("build/storage-namespace")
    logs.mkdir(parents=True, exist_ok=True)
    checker = logs / "strata_check"
    subprocess.run([tool("clang"), "-Wall", "-Wextra", "-O2", "tools/strata_check.c", "-o", str(checker)],
                   check=True, timeout=30)
    with fresh_disk() as disk:
        cmd = command(BOOT_IMAGE, disk, cpus=4)
        for action, text in (
            ("namespace", b"[PASS] StrataFS directory rename and hard links committed"),
            ("reopen", b"[PASS] StrataFS namespace survived reboot and was reclaimed"),
        ):
            with running(cmd, logs / f"{action}.log") as guest:
                guest.expect(PROMPT)
                require(text, shell(guest, f"/bin/fs-campaign {action}"))
                guest.interrupt()
            with (logs / f"{action}-check.log").open("w") as raw:
                subprocess.run([str(checker), str(disk)], stdout=raw, stderr=subprocess.STDOUT,
                               check=True, timeout=30)
        with running(cmd, logs / "orphan-stop.log") as guest:
            guest.expect(PROMPT)
            guest.send(b"/bin/fs-campaign orphan\r")
            guest.expect("[PASS] final close reclaimed the unlinked inode")
            guest.expect("[READY] orphan survived unlink with last descriptor open")
            guest.interrupt()
        with running(cmd, logs / "orphan-reboot.log") as guest:
            guest.expect("Reclaimed 1 unlinked open inode(s) after recovery.")
            guest.expect(PROMPT)
            require(b"err=-2", shell(guest, "stat /orphan"))
        with (logs / "orphan-check.log").open("w") as raw:
            subprocess.run([str(checker), str(disk)], stdout=raw, stderr=subprocess.STDOUT,
                           check=True, timeout=30)
        require(b"1 reachable inodes", (logs / "orphan-check.log").read_bytes())
    print("[PASS] Directory moves, hard-link identity, open-unlinked recovery, exact reboot contents, and raw namespace checks")


if __name__ == "__main__":
    main()
