#!/usr/bin/env python3
"""Check cold and cached data, eviction, write coherence and allocation rollback."""

import errno
from pathlib import Path
import subprocess
import tempfile

from test_smoke import PROMPT, require, shell
from vm import BOOT_IMAGE, boot_profile, command, fresh_disk, running, tool


def main():
    logs = Path("build/storage-cache")
    logs.mkdir(parents=True, exist_ok=True)
    checker = logs / "strata_check"
    subprocess.run([tool("clang"), "-O2", "tools/strata_check.c", "-o", str(checker)], check=True, timeout=30)
    with tempfile.TemporaryDirectory(dir="build") as scratch:
        fault_image = boot_profile(BOOT_IMAGE, Path(scratch), "storage=alloc-fail")
        for action, boot, marker in (
            ("cache", BOOT_IMAGE, b"[PASS] StrataFS cache hits="),
            ("alloc-fail", fault_image, b"[PASS] StrataFS allocation rollback and exact retry bytes"),
        ):
            with fresh_disk() as disk:
                with running(command(boot, disk, cpus=4), logs / f"{action}.log") as guest:
                    guest.expect(PROMPT)
                    result = shell(guest, f"/bin/fs-campaign {action}")
                    require(marker, result)
                    if action == "alloc-fail":
                        require(b"occurrence=2", result)
                    else:
                        require(b"[PASS] Block MSI-X woke", result)
                with (logs / f"{action}-checker.log").open("w") as raw:
                    subprocess.run([str(checker), str(disk)], stdout=raw, stderr=subprocess.STDOUT,
                                   check=True, timeout=30)
                require(b"1 reachable inodes", (logs / f"{action}-checker.log").read_bytes())
        rules = logs / "flush-blkdebug.conf"
        rules.write_text(f'[inject-error]\nevent = "flush_to_disk"\niotype = "flush"\n'
                         f'errno = "{errno.EIO}"\nonce = "on"\n', encoding="ascii")
        with fresh_disk() as disk:
            base = command(BOOT_IMAGE, disk, cpus=4)
            with running(base, logs / "flush-prepare.log") as guest:
                guest.expect(PROMPT)
                if shell(guest, "write /flush-proof stable"):
                    raise RuntimeError("flush fixture write failed")
            with running(command(BOOT_IMAGE, disk, cpus=4, block_debug=rules), logs / "flush-error.log") as guest:
                guest.expect(PROMPT)
                require(b"Backend flush failure surfaced as EIO and stopped further commits",
                        shell(guest, "/bin/fs-campaign flush-retry"))
            with running(base, logs / "flush-reboot.log") as guest:
                guest.expect(PROMPT)
                if shell(guest, "cat /flush-proof") != b"stable\r\n":
                    raise RuntimeError("failed transaction changed the recovered file")
                require(b"StrataFS accepted exact retry after remount", shell(guest, "/bin/fs-campaign flush-recovered"))
            with running(base, logs / "flush-confirmed.log") as guest:
                guest.expect(PROMPT)
                if shell(guest, "cat /flush-proof") != b"stable\r\nagain\r\n":
                    raise RuntimeError("retried transaction did not survive reboot")
            with (logs / "flush-checker.log").open("w") as raw:
                subprocess.run([str(checker), str(disk)], stdout=raw, stderr=subprocess.STDOUT,
                               check=True, timeout=30)
    print("[PASS] StrataFS cache pressure, coherent writes, and recovery from a real allocation rollback")


if __name__ == "__main__":
    main()
