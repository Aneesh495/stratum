#!/usr/bin/env python3
"""Exercise real ring-3 exec, shell pipelines and redirection on 4 and 8 CPUs."""

import argparse
from pathlib import Path

from test_smoke import PROMPT, require, shell
from vm import BOOT_IMAGE, command, fresh_disk, running


def check(cpus, log_dir):
    with fresh_disk() as disk:
        with running(command(BOOT_IMAGE, disk, cpus=cpus), log_dir / f"{cpus}cpu.log") as guest:
            guest.expect(PROMPT)
            require(b"upper\r\n", shell(guest, "ls /bin"))
            require(b"one | two\r\n", shell(guest, "echo 'one | two'"))
            require(b"ONE | TWO\r\n", shell(guest, "echo one \\| two|upper"))
            require(b"invalid command syntax", shell(guest, "echo 'unterminated"))
            for line in ('echo "first line" > /pipe', "echo second >>/pipe", "upper </pipe >/upper"):
                if shell(guest, line):
                    raise RuntimeError(f"unexpected output for {line!r}")
            if shell(guest, "cat /upper") != b"FIRST LINE\r\nSECOND\r\n":
                raise RuntimeError("redirection data mismatch")
            guest.send(b"/bin/exec-campaign\r")
            output = guest.expect(PROMPT, timeout=300)
            require(b"[PASS] fork/exec/wait cycles=10000 with argv, envp, CLOEXEC and resource recovery", output)
    print(f"[PASS] {cpus} CPUs: shell pipelines, redirection, and 10000 guest fork/exec/wait cycles", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--log-dir", type=Path, default=Path("build/exec"))
    args = parser.parse_args()
    for cpus in (4, 8):
        check(cpus, args.log_dir)


if __name__ == "__main__":
    main()
