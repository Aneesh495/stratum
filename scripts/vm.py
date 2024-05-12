"""Shared QEMU configuration and serial I/O for host-side guest tests."""

from contextlib import contextmanager
from pathlib import Path
import os
import select
import shutil
import subprocess
import tempfile
import time


BOOT_IMAGE = Path("build/images/stratum-boot.img")
DATA_IMAGE = Path("build/images/stratum-data.img")
FIRMWARE_PATHS = (
    "/opt/homebrew/share/qemu/edk2-x86_64-code.fd",
    "/usr/share/OVMF/OVMF_CODE_4M.fd",
    "/usr/share/OVMF/OVMF_CODE.fd",
    "/usr/share/qemu/edk2-x86_64-code.fd",
)


def tool(name):
    override = {"qemu-system-x86_64": "QEMU", "ld.lld": "LD_LLD"}.get(name, name.replace("-", "_").upper())
    return os.environ.get(override) or shutil.which(name) or name


def firmware():
    return os.environ.get("OVMF") or next(
        (path for path in FIRMWARE_PATHS if Path(path).is_file()), FIRMWARE_PATHS[0]
    )


def command(boot, data=None, *, cpus=4, memory="256M", ovmf=None, host_port=None, ethernet=None, mac=None, capture=None, block_debug=None):
    cmd = [
        os.environ.get("QEMU") or tool("qemu-system-x86_64"),
        "-machine", "q35,accel=tcg", "-cpu", "qemu64,+nx,+apic",
        "-smp", str(cpus), "-m", memory,
        "-drive", f"if=pflash,format=raw,readonly=on,file={ovmf or firmware()}",
        "-drive", f"file={boot},if=none,id=bootdisk,format=raw,snapshot=on",
        "-device", "ide-hd,drive=bootdisk,bootindex=1",
        "-display", "none", "-monitor", "none", "-no-reboot",
    ]
    if data is not None:
        backing = f"blkdebug:{block_debug}:{data}" if block_debug else data
        cmd += [
            "-drive", f"file={backing},if=none,id=datadisk,format=raw",
            "-device", "virtio-blk-pci,drive=datadisk,disable-legacy=on",
        ]
    netdev = "user,id=net0,restrict=on"
    if host_port is not None:
        netdev += f",hostfwd=tcp:127.0.0.1:{host_port}-:9090"
    if ethernet is not None:
        netdev = f"socket,id=net0,{ethernet}"
    nic = "virtio-net-pci,netdev=net0,disable-legacy=on"
    if mac is not None:
        nic += f",mac={mac}"
    cmd += ["-netdev", netdev, "-device", nic]
    if capture is not None:
        cmd += ["-object", f"filter-dump,id=wire,netdev=net0,file={capture}"]
    return cmd


def boot_profile(image, directory, cmdline):
    boot = directory / BOOT_IMAGE.name
    shutil.copyfile(image, boot)
    config = directory / "cmdline.txt"
    config.write_text(cmdline + "\n", encoding="ascii")
    subprocess.run([tool("mcopy"), "-o", "-i", f"{boot}@@1M",
                    str(config), "::/cmdline.txt"], check=True, timeout=10)
    return boot


class Guest:
    """Read serial bytes against an absolute deadline and retain the full transcript."""

    def __init__(self, cmd):
        self.proc = subprocess.Popen(
            cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, bufsize=0,
        )
        self.transcript = bytearray()
        self.cursor = 0

    def expect(self, marker, timeout=45, *, allow_panic=False):
        expected = marker.encode()
        deadline = time.monotonic() + timeout
        while True:
            start = self.cursor
            end = self.transcript.find(expected, start)
            panic = self.transcript.find(b"KERNEL PANIC:", start)
            if not allow_panic and panic >= 0 and (end < 0 or panic < end):
                raise RuntimeError(f"guest panic before {marker!r}")
            if end >= 0:
                self.cursor = end + len(expected)
                return bytes(self.transcript[start:self.cursor])
            remaining = deadline - time.monotonic()
            if remaining <= 0 or not select.select([self.proc.stdout], [], [], remaining)[0]:
                raise TimeoutError(f"guest did not produce {marker!r} within {timeout}s")
            chunk = os.read(self.proc.stdout.fileno(), 65536)
            if not chunk:
                raise RuntimeError(f"QEMU exited before {marker!r} (status {self.proc.wait()})")
            self.transcript.extend(chunk)

    def send(self, data):
        self.proc.stdin.write(data)

    def interrupt(self):
        self.proc.kill()
        self.proc.wait(timeout=5)

    def close(self):
        if self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.interrupt()
        self.proc.stdin.close()
        self.proc.stdout.close()


@contextmanager
def running(cmd, log):
    guest = Guest(cmd + ["-serial", "stdio"])
    try:
        yield guest
    finally:
        guest.close()
        log.parent.mkdir(parents=True, exist_ok=True)
        log.write_bytes(guest.transcript)


@contextmanager
def fresh_disk():
    with tempfile.TemporaryDirectory(prefix="stratum-", dir="build") as directory:
        path = Path(directory) / DATA_IMAGE.name
        shutil.copyfile(DATA_IMAGE, path)
        yield path
