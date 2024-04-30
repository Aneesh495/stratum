#!/usr/bin/env python3
"""Exercise guest pipes and message channels under backpressure and readiness races."""

from pathlib import Path

from test_smoke import PROMPT, require
from vm import BOOT_IMAGE, command, running


def payload_hash(channel):
    result = 0xcbf29ce484222325
    for sequence in range(50000):
        length = (32 + sequence % 481) if channel else (768 + sequence % 32)
        initial = (sequence ^ (sequence // 256)) * 13
        for i in range(length):
            byte = (initial + 17 * i + i // 256) % 256
            result = ((result ^ byte) * 0x100000001b3) % (1 << 64)
    return f"{result:x}"


def check_transcript(output, pipe_hash, channel_hash):
    require(f"[PASS] pipe transfers=50000 payload_fnv64={pipe_hash}".encode(), output)
    require(f"[PASS] channel transfers=50000 payload_fnv64={channel_hash}".encode(), output)
    require(b"[PASS] readiness cases=25000 data=6250 peer_close=6250 descriptor_replace=6250 timeout=6250", output)
    require(b"[PASS] IPC descriptors and free pages reconciled", output)


def main():
    pipe_hash, channel_hash = payload_hash(False), payload_hash(True)
    for cpus in (4, 8):
        with running(command(BOOT_IMAGE, cpus=cpus), Path(f"build/ipc/{cpus}cpu.log")) as guest:
            guest.expect(PROMPT)
            guest.send(b"/bin/ipc-campaign\r")
            check_transcript(guest.expect(PROMPT, timeout=180), pipe_hash, channel_hash)
        print(f"[PASS] {cpus} CPUs: 100000 IPC transfers, 25000 readiness races, descriptors and pages recovered", flush=True)


if __name__ == "__main__":
    main()
