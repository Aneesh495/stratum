#!/usr/bin/env python3
"""Run the fixed boot matrix and exercise the real loader and handoff validators."""

import argparse
from contextlib import nullcontext
from concurrent.futures import ThreadPoolExecutor
from hashlib import sha256
import json
from pathlib import Path
import re
import socket
import struct
import subprocess
import tempfile
import time

from test_boot import run as test_loader
from test_smoke import PROMPT, require, shell
from vm import BOOT_IMAGE, DATA_IMAGE, boot_profile, command, firmware, fresh_disk, running, tool


BOOT_MAGIC = 0x5354524154554D31
KERNEL = Path("build/stratum.elf")
LIMIT = 1 << 32
U64_MAX = (1 << 64) - 1

# The sizes and offsets are the version-1 wire layout in shared/boot_info.h.
HANDOFF_GROUPS = (
    ("magic", 0, "Q", (0, 1, BOOT_MAGIC ^ 0xFF, BOOT_MAGIC >> 8, U64_MAX, 0x5354524154554D32, 0x314D555441525453, 0x8000000000000000), 3),
    ("version", 8, "I", (0, 2, 3, 255, 256, 65535, 1 << 31, LIMIT - 1), 4),
    ("header-size", 12, "I", (0, 16, 112, 367, 369, 512, 1 << 31, LIMIT - 1), 5),
    ("map-count", 56, "I", (0, 513, 514, 1024, 4096, 65535, 1 << 31, LIMIT - 1), 6),
    ("map-stride", 60, "I", (0, 1, 16, 31, 33, 48, 4096, LIMIT - 1), 6),
    ("map-address", 48, "Q", (0, LIMIT - 1, LIMIT - 2, LIMIT - 31, LIMIT, LIMIT + 4096, 1 << 63, U64_MAX), 6),
    ("kernel-physical", 64, "Q", (0, 1, 4095, 4097, LIMIT - 1, LIMIT, 1 << 63, U64_MAX), 7),
    ("kernel-size", 72, "Q", (0, 1, 4095, 4097, (64 << 20) + 4096, LIMIT, 1 << 63, U64_MAX), 7),
    ("kernel-virtual", 80, "Q", (0, 1, 0x400000, 0xffffffff7fffffff, 0xffffffff80000001, 0xffffffff80200000, 1 << 63, U64_MAX), 7),
    ("kernel-entry", 88, "Q", (0, 1, 0x400000, 0xffffffff7fffffff, 0xffffffff84000000, 0xffffffffc0000000, 1 << 63, U64_MAX), 7),
    ("initramfs-address", 96, "Q", (0, LIMIT - 1, LIMIT - 2, LIMIT - 3, LIMIT, LIMIT + 4096, 1 << 63, U64_MAX), 8),
    ("rsdp-address", 40, "Q", (LIMIT - 1, LIMIT - 2, LIMIT - 35, LIMIT, LIMIT + 1, LIMIT + 4096, 1 << 63, U64_MAX), 8),
    ("cmdline-terminator", 367, "B", (1, 9, 10, 13, 32, 127, 128, 255), 10),
    ("descriptor-pages", 24, "Q", (0, 1 << 52, (1 << 52) + 1, 1 << 56, 1 << 63, U64_MAX - 1, U64_MAX), 11),
)


class Debugger:
    """Minimal GDB remote transport for a local, paused QEMU instance."""

    def __init__(self, sock):
        self.sock = sock
        self.input = sock.makefile("rb", buffering=0)

    def close(self):
        self.input.close()
        self.sock.close()

    def read_exact(self, size):
        data = bytearray()
        while len(data) < size:
            chunk = self.input.read(size - len(data))
            if not chunk:
                raise RuntimeError("QEMU debugger disconnected")
            data.extend(chunk)
        return bytes(data)

    def request(self, command):
        payload = command.encode("ascii")
        self.sock.sendall(b"$" + payload + b"#" + f"{sum(payload) & 255:02x}".encode())
        if self.read_exact(2) != b"+$":
            raise RuntimeError(f"QEMU debugger rejected packet {command!r}")
        data = bytearray()
        while (byte := self.read_exact(1)) != b"#":
            data.extend(byte)
        if int(self.read_exact(2), 16) != sum(data) & 255:
            raise RuntimeError("QEMU debugger response checksum mismatch")
        self.sock.sendall(b"+")
        return bytes(data)

    def memory(self, address, size):
        data = bytes.fromhex(self.request(f"m{address:x},{size:x}").decode())
        if len(data) != size:
            raise RuntimeError("QEMU debugger returned incomplete memory")
        return data

    def replace(self, address, data):
        if self.request(f"M{address:x},{len(data):x}:{data.hex()}") != b"OK":
            raise RuntimeError("could not inject handoff mutation")


def connect(port):
    deadline = time.monotonic() + 5
    while True:
        try:
            return Debugger(socket.create_connection(("127.0.0.1", port), timeout=35))
        except ConnectionRefusedError:
            if time.monotonic() >= deadline:
                raise
            time.sleep(0.02)


def digest(path):
    with path.open("rb") as stream:
        return sha256(stream.read()).hexdigest()


def cpu_result(cpu):
    value = 0x5354524100000000 | cpu
    for i in range(128):
        value = ((value ^ i) * 1099511628211) & U64_MAX
    return f"{value:x}"


def valid_boot(boot, cpus, memory, repeat, logs):
    tag = f"{cpus}cpu-{memory}-{repeat}"
    degraded = memory == "64M"
    with (nullcontext() if degraded else fresh_disk()) as disk:
        with running(command(boot, disk, cpus=cpus, memory=memory), logs / f"{tag}.log") as guest:
            output = guest.expect(PROMPT, timeout=65)
            require(b"Boot handoff verified successfully", output)
            require(b"Root mount failed" if degraded else b"Mounted virtio-blk at '/'", output)
            probes = re.findall(rb"^\[BOOT-PROBE\] cpu=(\d+) result=([0-9a-f]+)\r?$", output, re.M)
            expected = [(str(i).encode(), cpu_result(i).encode()) for i in range(cpus)]
            if probes != expected:
                raise RuntimeError(f"{tag}: CPU probes {probes!r} != {expected!r}")
            require(f"Online CPUs:  {cpus}\r\n".encode(), shell(guest, "sysinfo"))
    return tag


def debugger_boot(entry, logs, case=None):
    tag = case[0] if case else "debugger-entry"
    with socket.socket() as port_socket:
        port_socket.bind(("127.0.0.1", 0))
        port = port_socket.getsockname()[1]
    args = command(BOOT_IMAGE, cpus=1) + ["-S", "-gdb", f"tcp:127.0.0.1:{port}"]
    with running(args, logs / f"{tag}.log") as guest:
        debugger = connect(port)
        try:
            if debugger.request(f"Z1,{entry:x},1") != b"OK" or debugger.request("c")[:1] not in (b"T", b"S"):
                raise RuntimeError("QEMU did not stop at the kernel entry")
            registers = bytes.fromhex(debugger.request("g").decode())
            rsi, rdi = struct.unpack_from("<QQ", registers, 4 * 8)
            rip, = struct.unpack_from("<Q", registers, 16 * 8)
            header = debugger.memory(rdi, 112)
            magic, version, size = struct.unpack_from("<QII", header)
            if rip != entry or rsi != BOOT_MAGIC or magic != rsi or version != 1 or size != 368:
                raise RuntimeError(f"wrong debugger entry state: RIP={rip:x} RSI={rsi:x} RDI={rdi:x}")
            result = {"rip": rip, "rdi": rdi, "rsi": rsi, "handoff": header.hex()}
            if case:
                _, group, offset, form, value, code = case
                base = struct.unpack_from("<Q", header, 48)[0] if group == "descriptor-pages" else rdi
                data = struct.pack("<" + form, value)
                previous = debugger.memory(base + offset, len(data))
                if previous == data:
                    raise RuntimeError(f"mutation {tag} does not change the handoff")
                debugger.replace(base + offset, data)
                result.update(address=base + offset, original=previous.hex(), mutation=data.hex(), error=-code)
            if debugger.request(f"z1,{entry:x},1") != b"OK" or debugger.request("D") != b"OK":
                raise RuntimeError("could not detach QEMU debugger")
        finally:
            debugger.close()
        if case:
            guest.expect(f"Boot handoff validation failed with code -{code}\r\n", allow_panic=True)
        else:
            guest.expect(PROMPT)
    return result


def cases():
    return [(f"handoff-{group}-{index}", group, offset, form, value, code)
            for group, offset, form, values, code in HANDOFF_GROUPS
            for index, value in enumerate(values)]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--jobs", type=int, default=2)
    parser.add_argument("--debugger-only", action="store_true", help="verify the entry probe without the campaign")
    args = parser.parse_args()
    if not 1 <= args.jobs <= 8:
        parser.error("--jobs must be between 1 and 8")
    logs = Path("build/boot-campaign")
    logs.mkdir(parents=True, exist_ok=True)
    with KERNEL.open("rb") as elf:
        entry, = struct.unpack_from("<Q", elf.read(32), 24)
    evidence = {
        "kernel": digest(KERNEL), "boot_image": digest(BOOT_IMAGE), "data_image": digest(DATA_IMAGE),
        "firmware": digest(Path(firmware())),
        "qemu": subprocess.check_output([tool("qemu-system-x86_64"), "--version"], text=True, timeout=10).splitlines()[0],
        "entry": debugger_boot(entry, logs),
    }
    print("[PASS] debugger observed the ELF entry, RDI handoff, and RSI magic", flush=True)
    if args.debugger_only:
        (logs / "entry.json").write_text(json.dumps(evidence, indent=2) + "\n")
        return

    profiles = [(cpu, memory) for memory in ("256M", "1G") for cpu in (1, 2, 4, 8)] + [(1, "64M")]
    with tempfile.TemporaryDirectory(dir="build") as directory, ThreadPoolExecutor(args.jobs) as pool:
        boot = boot_profile(BOOT_IMAGE, Path(directory), "boot-probe")
        evidence["probe_image"] = digest(boot)
        futures = [pool.submit(valid_boot, boot, cpus, memory, repeat, logs)
                   for cpus, memory in profiles for repeat in range(1, 6)]
        evidence["fresh_boots"] = [future.result() for future in futures]
        print("[PASS] 45 fresh boots with verified per-CPU work", flush=True)

        futures = {case[0]: pool.submit(debugger_boot, entry, logs, case) for case in cases()}
        evidence["handoff_rejections"] = {tag: future.result() for tag, future in futures.items()}
        print(f"[PASS] {len(futures)} rejected handoff mutations in real guests", flush=True)

    evidence["loader_rejections"] = test_loader(logs / "loader")
    if len(evidence["fresh_boots"]) != 45 or len(evidence["handoff_rejections"]) != 111 or len(evidence["loader_rejections"]) != 17:
        raise RuntimeError("incomplete boot workload")
    (logs / "results.json").write_text(json.dumps(evidence, indent=2) + "\n")
    print("[PASS] A01 boot workload: 45 fresh boots and 128 rejected loader/handoff inputs", flush=True)


if __name__ == "__main__":
    main()
