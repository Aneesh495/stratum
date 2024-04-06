#!/usr/bin/env python3
"""Confirm that a valid regression bundle survives deliberate negative controls."""

import argparse
from hashlib import sha256
import json
import os
from pathlib import Path
import shutil
import tempfile

from verify_evidence import verify


def overwrite(path, value):
    staged = path.with_name(path.name + ".new")
    staged.write_bytes(value)
    staged.replace(path)


def update_bytes(root, manifest, name, before, after, *, rehash=True):
    path = root / name
    data = path.read_bytes()
    if before not in data:
        raise RuntimeError(f"control target missing from {name}")
    changed = data.replace(before, after)
    overwrite(path, changed)
    if rehash:
        manifest["files"][name] = sha256(changed).hexdigest()


def controls():
    return [
        ("schema", "unsupported schema", lambda _, m: m.update(schema=99)),
        ("source", "source commit does not match", lambda _, m: m.update(source_commit="0" * 40)),
        ("tree", "source tree does not match", lambda _, m: m.update(source_tree="0" * 40)),
        ("missing workload", "missing or unexpected workload", lambda _, m: m["commands"].pop("recovery")),
        ("failed exit", "ledger: unsuccessful exit", lambda _, m: m["commands"]["ledger"].update(exit=1)),
        ("absent duration", "replication: invalid duration", lambda _, m: m["commands"]["replication"].update(seconds=0)),
        ("full acceptance claim", "cannot certify full acceptance", lambda _, m: m.update(full_acceptance="passed")),
        ("missing transcript", "missing, empty, or unsafe path",
         lambda r, _: (r / "transcripts/recovery/replayed.log").unlink()),
        ("changed bytes", "content digest mismatch",
         lambda r, m: update_bytes(r, m, "logs/ledger.log", b"[PASS]", b"[FAIL]", rehash=False)),
        ("lower mutation count", "host operation counts missing",
         lambda r, m: update_bytes(r, m, "logs/host.log", b"350000 iterations", b"1 iterations")),
        ("wrong CPU count", "8cpu-1G: shell or recovery mismatch",
         lambda r, m: update_bytes(r, m, "transcripts/smoke/8cpu-1G-boot.log",
                                   b"Online CPUs:  8", b"Online CPUs:  7")),
        ("lost replay observation", "WAL recovery evidence mismatch",
         lambda r, m: update_bytes(r, m, "transcripts/recovery/replayed.log", b"Replayed committed", b"Read")),
        ("wrong replica payload", "missing follower recovery payload",
         lambda r, m: update_bytes(r, m, "transcripts/replication/follower-reboot.log", b"via-ethernet", b"damaged")),
        ("mismatched build", "independent build outputs differ",
         lambda r, m: update_bytes(r, m, "artifacts/build-b/BOOTX64.EFI", b"MZ", b"QZ")),
    ]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("bundle", type=Path)
    args = parser.parse_args()
    original = args.bundle.resolve()
    verify(original)
    for name, expected, mutate in controls():
        with tempfile.TemporaryDirectory(dir=original.parent) as directory:
            copy = Path(directory) / "bundle"
            shutil.copytree(original, copy, copy_function=os.link)
            path = copy / "manifest.json"
            manifest = json.loads(path.read_bytes())
            mutate(copy, manifest)
            overwrite(path, json.dumps(manifest).encode())
            try:
                verify(copy)
            except ValueError as error:
                if expected not in str(error):
                    raise RuntimeError(f"{name}: wrong rejection: {error}") from error
            else:
                raise RuntimeError(f"{name}: verifier accepted an invalid bundle")
            print(f"[PASS] rejected {name}: {expected}", flush=True)
    verify(original)
    print(f"[PASS] Original verified before and after {len(controls())} controls.")


if __name__ == "__main__":
    main()
