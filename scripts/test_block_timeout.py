#!/usr/bin/env python3
"""Make a real QEMU block request miss its deadline and verify queue recovery."""

import os
from pathlib import Path
import tempfile

from qmp import monitor
from test_smoke import PROMPT, require, shell
from vm import BOOT_IMAGE, command, fresh_disk, running


def throttle(qmp, bytes_per_second):
    qmp("block_set_io_throttle", {"device": "datadisk", "bps": 0, "bps_rd": bytes_per_second,
                                 "bps_wr": 0, "iops": 0, "iops_rd": 0, "iops_wr": 0})


def main():
    logs = Path("build/block-timeout")
    logs.mkdir(parents=True, exist_ok=True)
    for cpus in (2, 4):
        with tempfile.TemporaryDirectory(prefix="qmp-", dir="build") as work, fresh_disk() as disk:
            path = os.path.relpath(Path(work) / "monitor")
            cmd = command(BOOT_IMAGE, disk, cpus=cpus) + ["-qmp", f"unix:{path},server=on,wait=off"]
            with running(cmd, logs / f"{cpus}cpu.log") as guest, monitor(path, logs / f"{cpus}cpu-qmp.jsonl") as qmp:
                guest.expect(PROMPT)
                require(b"[READY] Block timeout fixture persisted", shell(guest, "/bin/fs-campaign timeout-prepare"))
                throttle(qmp, 512)
                guest.send(b"/bin/fs-campaign timeout-read\r")
                guest.expect("[VIRTIO-BLK] Request timed out", timeout=30)
                throttle(qmp, 0)
                reply = guest.expect(PROMPT, timeout=30)
                require(b"[PASS] Block timeout returned a bounded read error", reply)
                require(b"MSI-X completion vector", reply)
                require(b"[PASS] Block reset restored exact fixture bytes", shell(guest, "/bin/fs-campaign timeout-retry"))
        print(f"[PASS] {cpus} CPUs: timed-out virtio DMA returned a bounded error and reset restored the disk")


if __name__ == "__main__":
    main()
