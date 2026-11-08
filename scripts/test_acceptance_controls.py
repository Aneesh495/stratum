#!/usr/bin/env python3
"""Exercise the full verifier on isolated corruptions of a recorded bundle."""

import argparse
import json
import os
from pathlib import Path
import re
import shutil
import tempfile

from evidence_format import file_hash
from test_evidence import overwrite
from verify_acceptance import document, verify


def artifact(root, workload, basename):
    names = document(root / "workloads.json")[workload]["artifacts"]
    paths = [root / name for name in names if Path(name).name == basename]
    if len(paths) != 1:
        raise RuntimeError(f"{workload}: missing or ambiguous {basename}")
    return paths[0]


def change_file(root, manifest, path, transform, *, rehash=True):
    data = path.read_bytes()
    changed = transform(data)
    if changed == data:
        raise RuntimeError(f"control did not change {path}")
    overwrite(path, changed)
    if rehash:
        manifest["files"][str(path.relative_to(root))] = file_hash(path)


def change_json(root, manifest, path, mutate):
    def transform(raw):
        row = json.loads(raw)
        mutate(row)
        return json.dumps(row).encode()

    change_file(root, manifest, path, transform)


def controls():
    return (
        ("manifest schema", "unsupported acceptance manifest schema", lambda r, m: m.update(schema=99)),
        ("wrong commit", "acceptance source commit mismatch", lambda r, m: m.update(source="0" * 40)),
        ("missing transcript", "missing or extra acceptance artifacts",
         lambda r, m: artifact(r, "boot", "1cpu-256M-1.log").unlink()),
        ("altered bytes", "acceptance artifact digest mismatch",
         lambda r, m: change_file(r, m, r / "logs/boot.log", lambda raw: raw + b"x", rehash=False)),
        ("missing CPU observation", "missing CPU probe",
         lambda r, m: change_file(r, m, artifact(r, "boot", "1cpu-256M-1.log"),
                                  lambda raw: re.sub(rb"^\[BOOT-PROBE\].*\n", b"", raw, flags=re.M))),
        ("zero completed fuzz operations", "invalid fuzz duration, coverage or execution count",
         lambda r, m: change_json(r, m, artifact(r, "production-fuzz", "results.json"),
                                  lambda row: row["targets"]["elf"].update(executions=0))),
        ("inflated live population", "wrong live population",
         lambda r, m: change_file(r, m, artifact(r, "threads", "4cpu.log"),
                                  lambda raw: re.search(rb"^\[LIVE\].*\n", raw, re.M)[0] + raw)),
        ("shortened soak", "soak profile, count or duration mismatch",
         lambda r, m: change_json(r, m, artifact(r, "soak", "results.json"),
                                  lambda row: row.update(elapsed_seconds=3599))),
        ("missing recovery check", "missing independent follower disk check",
         lambda r, m: change_file(r, m, artifact(r, "service", "follower-check.log"),
                                  lambda raw: raw.replace(b"[PASSED]", b"[FAILED]"))),
        ("failed process exit", "service: command mismatch",
         lambda r, m: change_json(r, m, r / "workloads.json", lambda row: row["service"].update(exit=1))),
        ("skipped required workload", "missing or extra acceptance workloads",
         lambda r, m: change_json(r, m, r / "workloads.json", lambda row: row.pop("storage-mixed"))),
        ("mismatched application payload", "missing or duplicate durable service acknowledgments",
         lambda r, m: change_file(r, m, artifact(r, "service", "follower.log"),
                                  lambda raw: raw.replace(b"[RECORD] entry0000 durable", b"[RECORD] entry0001 durable"))),
        ("missing benchmark samples", "missing host boot samples",
         lambda r, m: change_json(r, m, artifact(r, "guest-bench", "samples.json"),
                                  lambda row: row["profiles"]["1"]["host_boot_ns"].pop())),
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("bundle", type=Path)
    original = parser.parse_args().bundle.resolve()
    complete = verify(original, require_complete=False)
    results = []
    for name, expected, mutate in controls():
        with tempfile.TemporaryDirectory(dir=original.parent) as directory:
            bundle = Path(directory) / "bundle"
            shutil.copytree(original, bundle, copy_function=os.link)
            path = bundle / "MANIFEST.json"
            manifest = document(path)
            mutate(bundle, manifest)
            overwrite(path, json.dumps(manifest).encode())
            try:
                verify(bundle, require_complete=False)
            except (ValueError, RuntimeError) as error:
                if expected not in str(error):
                    raise RuntimeError(f"{name}: unexpected rejection: {error}") from error
                results.append({"control": name, "rejected": str(error)})
            else:
                raise RuntimeError(f"{name}: verifier accepted altered evidence")
            print(f"[PASS] {name}: {expected}", flush=True)
    verify(original, require_complete=False)
    destination = original.with_name(original.name + "-verifier-controls.json")
    destination.write_text(json.dumps({
        "source_manifest_sha256": file_hash(original / "MANIFEST.json"),
        "original_complete": complete, "controls": results,
    }, indent=2) + "\n", encoding="utf-8")
    print(f"Original verified before and after {len(results)} controls. Results: {destination}")


if __name__ == "__main__":
    main()
