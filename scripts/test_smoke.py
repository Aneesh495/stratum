#!/usr/bin/env python3
"""Exercise the normal ring-3 serial shell and a real disk across a VM interruption."""

import argparse
from pathlib import Path
import subprocess

from vm import BOOT_IMAGE, command, fresh_disk, running, tool


PROMPT = "stratum> "


def shell(guest, line):
    guest.send(line.encode() + b"\r")
    reply = guest.expect(PROMPT, timeout=10)
    echo, body = reply.split(b"\r\n", 1)
    if echo != line.encode():
        raise RuntimeError(f"unexpected shell echo: {echo!r}")
    return body.removesuffix(PROMPT.encode())


def require(expected, actual):
    if expected not in actual:
        raise RuntimeError(f"expected {expected!r} in {actual!r}")


def test_profile(cpus, memory, log_dir, checker):
    name = f"{cpus}cpu-{memory}"
    with fresh_disk() as disk:
        cmd = command(BOOT_IMAGE, disk, cpus=cpus, memory=memory)
        with running(cmd, log_dir / f"{name}-boot.log") as guest:
            guest.expect(PROMPT)
            require(f"Online CPUs:  {cpus}\r\n".encode(), shell(guest, "sysinfo"))
            require(b"hello from ring three\r\n", shell(guest, "echo hello from ring three"))
            require(b"Directory '/proof' created.", shell(guest, "mkdir /proof"))
            if shell(guest, "write /proof/token durable-alpha"):
                raise RuntimeError("write reported an error")
            if shell(guest, "write /proof/token durable-beta"):
                raise RuntimeError("append reported an error")
            expected = b"durable-alpha\r\ndurable-beta\r\n"
            if shell(guest, "cat /proof/token") != expected:
                raise RuntimeError("payload mismatch before reboot")
            require(b"token\r\n", shell(guest, "ls /proof"))
            if shell(guest, "write /proof/other replace-me"):
                raise RuntimeError("replacement setup failed")
            if shell(guest, "mv /proof/token /proof/other"):
                raise RuntimeError("atomic replacement failed")
            if shell(guest, "mv /proof/other /moved"):
                raise RuntimeError("cross-directory rename failed")
            require(b"err=-2", shell(guest, "stat /proof/token"))
            require(b"err=-2", shell(guest, "stat /proof/other"))
            guest.interrupt()

        subprocess.run([checker, str(disk)], check=True, timeout=10)
        with running(cmd, log_dir / f"{name}-reboot.log") as guest:
            guest.expect(PROMPT)
            if shell(guest, "cat /moved") != expected:
                raise RuntimeError("durable renamed payload mismatch after reboot")
            require(b"moved\r\n", shell(guest, "ls /"))
            require(b"File '/moved' unlinked.", shell(guest, "rm /moved"))
            require(b"err=-2", shell(guest, "stat /moved"))
        subprocess.run([checker, str(disk)], check=True, timeout=10)
    print(f"[PASS] {name}: shell, disk interruption/reboot, payload, unlink, and raw disk checks", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--log-dir", type=Path, default=Path("build/smoke"))
    parser.add_argument("--matrix", action="store_true")
    args = parser.parse_args()
    checker = "build/strata_check"
    subprocess.run([tool("clang"), "-Wall", "-Wextra", "-O2",
                    "tools/strata_check.c", "-o", checker], check=True, timeout=30)
    profiles = ([(cpus, memory) for memory in ("256M", "1G") for cpus in (1, 2, 4, 8)]
                + [(1, "64M")]) if args.matrix else [(4, "256M")]
    for cpus, memory in profiles:
        test_profile(cpus, memory, args.log_dir, checker)


if __name__ == "__main__":
    main()
