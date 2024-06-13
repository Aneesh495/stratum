#!/usr/bin/env python3
"""Exercise the native Stratum TCP stack directly against a pinned Linux VM."""

import json
import os
from pathlib import Path
import subprocess
import tempfile

from build_cpio import make_entry
from ethernet_link import link
from evidence_format import file_hash
from test_tcp_campaign import server_completed, start_server, verify_wire
from vm import BOOT_IMAGE, command, running, tool


ISO_NAME = "alpine-virt-3.22.8-x86_64.iso"
ISO_SHA256 = "93ccf2243ee4fc19c78063774f92ff3d328390bae76bda5bfeaf42db46ce5059"
SESSION_COUNT = 32
PAYLOAD_SIZE = 65536

INIT = b"""#!/bin/busybox sh
set -eu
/bin/busybox mount -t proc proc /proc
/bin/busybox mount -t sysfs sysfs /sys
/bin/busybox modprobe virtio_pci
/bin/busybox modprobe virtio_net
/bin/busybox ip link set eth0 up
/bin/busybox ip addr add 10.0.2.16/24 dev eth0
exec /linux-tcp-reference
"""


def prepare(iso, scratch):
    if not iso.is_file():
        raise RuntimeError(f"set STRATUM_REFERENCE_ISO to the pinned {ISO_NAME} file")
    if file_hash(iso) != ISO_SHA256:
        raise RuntimeError("reference ISO does not match the pinned Alpine checksum")

    def extract(name):
        output = scratch / name
        with output.open("wb") as stream:
            subprocess.run(["tar", "-xOf", str(iso), f"boot/{name}"], stdout=stream, check=True, timeout=60)
        return output

    kernel, ramdisk = extract("vmlinuz-virt"), extract("initramfs-virt")
    binary = scratch / "reference"
    cache = scratch / "zig-cache"
    env = dict(os.environ, ZIG_LOCAL_CACHE_DIR=str(cache / "local"), ZIG_GLOBAL_CACHE_DIR=str(cache / "global"))
    subprocess.run([tool("zig"), "cc", "-target", "x86_64-linux-musl", "-static", "-O2", "-Wall", "-Wextra",
                    "tests/reference/linux_tcp.c", "-o", str(binary)], env=env, check=True, timeout=180)
    with ramdisk.open("ab") as stream:
        stream.write(bytes((-stream.tell()) % 4))
        stream.write(make_entry("stratum-reference-init", INIT, ino=1))
        stream.write(make_entry("linux-tcp-reference", binary.read_bytes(), ino=2))
        stream.write(make_entry("TRAILER!!!", mode=0, ino=0))
    return kernel, ramdisk


def main():
    logs = Path("build/tcp-reference")
    logs.mkdir(parents=True, exist_ok=True)
    iso = Path(os.environ.get("STRATUM_REFERENCE_ISO") or Path("build/reference-alpine") / ISO_NAME).resolve()
    capture = logs / "ethernet.pcap"
    with tempfile.TemporaryDirectory(prefix="linux-", dir="build") as directory, link() as (_, (server_port, linux_port)):
        linux_kernel, ramdisk = prepare(iso, Path(directory))
        linux_cmd = [
            tool("qemu-system-x86_64"), "-machine", "q35,accel=tcg", "-m", "256M",
            "-kernel", str(linux_kernel), "-initrd", str(ramdisk), "-append",
            "console=ttyS0 rdinit=/stratum-reference-init net.ifnames=0 panic=1",
            "-display", "none", "-monitor", "none", "-no-reboot",
            "-netdev", f"socket,id=ref,connect=127.0.0.1:{linux_port}",
            "-device", "virtio-net-pci,netdev=ref,disable-legacy=on,mac=52:54:00:12:34:57",
        ]
        stratum_cmd = command(BOOT_IMAGE, cpus=2, ethernet=f"connect=127.0.0.1:{server_port}", capture=capture)
        with running(linux_cmd, logs / "linux.log") as linux:
            linux.expect("Linux TCP reference ready", timeout=90)
            with running(stratum_cmd, logs / "stratum.log") as guest:
                start_server(guest, SESSION_COUNT)
                linux.send(b"\n")
                linux.expect("[PASS] Linux TCP reference sessions=32 bytes=4194304", timeout=180)
                server_completed(guest, SESSION_COUNT, PAYLOAD_SIZE)
        report = {
            "iso_sha256": ISO_SHA256, "reference_kernel_sha256": file_hash(linux_kernel),
            "reference_initramfs_sha256": file_hash(ramdisk), "sessions": SESSION_COUNT,
            "bytes": SESSION_COUNT * PAYLOAD_SIZE * 2,
            "stratum_boot_image_sha256": file_hash(BOOT_IMAGE), "pcap_sha256": file_hash(capture),
            "wire_payload_sha256": verify_wire(capture, SESSION_COUNT, PAYLOAD_SIZE),
        }
    (logs / "results.json").write_text(json.dumps(report, indent=2) + "\n")
    print("[PASS] Direct Linux TCP reference: 32 sessions and 4194304 reconstructed Ethernet payload bytes")


if __name__ == "__main__":
    main()
