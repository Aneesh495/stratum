#!/usr/bin/env python3
"""Independent persistent-media model for StrataFS's one-transaction redo protocol."""

from hashlib import sha256
import random
import struct


SEEDS = 500
STAGES = (
    "before descriptor", "descriptor queued", "first payload queued", "payload durable",
    "commit queued", "commit durable", "published", "partial checkpoint",
    "home durable", "checkpointed",
)


class Media:
    def __init__(self, home):
        self.disk = {("home", block): data for block, data in home.items()}
        self.dirty = {}

    def write(self, region, index, data):
        self.dirty[region, index] = data

    def flush(self):
        self.disk.update(self.dirty)
        self.dirty.clear()

    def cut(self, rng):
        # Volatile device writes can be lost or become persistent before the cut.
        disk = self.disk.copy()
        disk.update((key, value) for key, value in self.dirty.items() if rng.getrandbits(1))
        return disk


def recover(disk, allowed):
    home = {index: data for (region, index), data in disk.items() if region == "home"}
    sequence = disk.get(("header", 0), 0)
    if not sequence:
        return home

    descriptor = disk.get(("log", 0), b"")
    if len(descriptor) < 8 or (len(descriptor) - 8) % 4:
        raise ValueError("invalid descriptor")
    observed, = struct.unpack_from("<Q", descriptor)
    targets = [target for target, in struct.iter_unpack("<I", descriptor[8:])]
    if observed != sequence or not targets or len(set(targets)) != len(targets) or not set(targets) <= allowed:
        raise ValueError("invalid sequence or targets")
    payloads = [disk.get(("log", slot)) for slot in range(1, len(targets) + 1)]
    if any(payload is None for payload in payloads):
        raise ValueError("missing journal block")
    expected = sha256(descriptor + b"".join(payloads)).digest()
    if disk.get(("commit", 0)) != expected:
        raise ValueError("invalid commit or payload checksum")

    # No home block changes until the complete published transaction validates.
    home.update(zip(targets, payloads))
    return home


def campaign(seed):
    rng = random.Random(seed)
    targets = rng.sample(range(580, 16384), rng.randrange(2, 9))
    before = {block: rng.randbytes(32) for block in targets}
    after = {block: rng.randbytes(32) for block in targets}
    payloads = list(after.values())
    sequence = seed + 1
    descriptor = struct.pack(f"<Q{len(targets)}I", sequence, *targets)
    commit = sha256(descriptor + b"".join(payloads)).digest()
    media = Media(before)

    def check(stage):
        disk = media.cut(rng)
        expected = after if stage >= 6 else before
        if recover(disk, set(targets)) != expected:
            raise RuntimeError(f"seed {seed}, {STAGES[stage]}: recovered the wrong home image")

    check(0)
    media.write("log", 0, descriptor)
    check(1)
    media.write("log", 1, payloads[0])
    check(2)
    for slot, payload in enumerate(payloads[1:], 2):
        media.write("log", slot, payload)
    media.flush()
    check(3)
    media.write("commit", 0, commit)
    check(4)
    media.flush()
    check(5)
    media.write("header", 0, sequence)
    media.flush()
    check(6)

    for corrupt_key, replacement in (
        (("log", 0), struct.pack("<QI", sequence, 1)),
        (("log", 1), bytes(32)),
        (("commit", 0), bytes(32)),
    ):
        disk = {**media.disk, corrupt_key: replacement}
        unchanged = disk.copy()
        try:
            recover(disk, set(targets))
        except ValueError:
            if disk != unchanged:
                raise RuntimeError("corrupt recovery changed the disk before validation")
        else:
            raise RuntimeError(f"seed {seed}: corrupt journal field {corrupt_key} was accepted")

    media.write("home", targets[0], payloads[0])
    check(7)
    for block in targets[1:]:
        media.write("home", block, after[block])
    media.flush()
    check(8)
    media.write("header", 0, 0)
    media.flush()
    check(9)


if __name__ == "__main__":
    for seed in range(SEEDS):
        campaign(seed)
    print(f"[PASS] WAL model validated {SEEDS * len(STAGES)} crash states and rejected {SEEDS * 3} corrupt states")
