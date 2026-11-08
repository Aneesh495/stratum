#!/usr/bin/env python3
"""Package a locally qualified Stratum build for subsequent publication."""

import argparse
import gzip
from hashlib import sha256
import io
import json
from pathlib import Path
import tarfile
import tempfile

from evidence_format import file_hash
from test_acceptance_controls import controls
from verify_acceptance import document, git, require, verify


def qualified(bundle):
    verify(bundle, require_complete=False)
    result = document(bundle / "ACCEPTANCE.json")
    require({key for key, row in result["gates"].items() if row["status"] != "passed"} == {"A12"} and
            result["gates"]["A12"]["status"] == "unverified",
            "release requires passing local runtime and reproducibility workloads")
    capabilities = document(bundle / "features.json")
    require({key for key, row in capabilities.items() if row["status"] != "passed"} == {"K64"} and
            capabilities["K64"]["status"] == "unverified",
            "release requires complete local capability coverage")
    controls_path = bundle.with_name(bundle.name + "-verifier-controls.json")
    recorded = document(controls_path)
    expected = {name: reason for name, reason, _ in controls()}
    observed = {row["control"]: row["rejected"] for row in recorded["controls"]}
    require(recorded["source_manifest_sha256"] == file_hash(bundle / "MANIFEST.json") and
            len(recorded["controls"]) == len(expected) and observed.keys() == expected.keys() and
            all(expected[name] in observed[name] for name in expected),
            "release requires all negative controls on this evidence manifest")
    return result["source"], controls_path


def write_archive(path, sources, info):
    descriptor = json.dumps(info, indent=2, sort_keys=True).encode() + b"\n"
    with path.open("wb") as output, gzip.GzipFile(fileobj=output, filename="", mode="wb", mtime=0) as compressed:
        with tarfile.open(fileobj=compressed, mode="w|", format=tarfile.PAX_FORMAT) as archive:
            for name, source in sorted(sources.items()):
                entry = tarfile.TarInfo(name)
                entry.size = source.stat().st_size
                entry.mode = 0o755 if name.startswith("source/") and source.stat().st_mode & 0o100 else 0o644
                with source.open("rb") as stream:
                    archive.addfile(entry, stream)
            entry = tarfile.TarInfo("RELEASE.json")
            entry.size = len(descriptor)
            entry.mode = 0o644
            archive.addfile(entry, io.BytesIO(descriptor))

    expected = info["files"] | {"RELEASE.json": sha256(descriptor).hexdigest()}
    seen = set()
    with tarfile.open(path, "r|gz") as archive:
        for entry in archive:
            require(entry.isfile() and entry.name not in seen and entry.name in expected,
                    "unexpected release archive member")
            digest = sha256()
            with archive.extractfile(entry) as stream:
                while block := stream.read(65536):
                    digest.update(block)
            require(digest.hexdigest() == expected[entry.name], f"release archive digest mismatch: {entry.name}")
            seen.add(entry.name)
    require(seen == expected.keys(), "release archive omitted required files")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("bundle", type=Path)
    bundle = parser.parse_args().bundle.resolve()
    source, controls_path = qualified(bundle)
    tracked = [Path(name.decode()) for name in git("ls-files", "-z").split(b"\0") if name]
    sources = {f"source/{path.as_posix()}": path for path in tracked}
    workload = document(bundle / "workloads.json")["image"]
    for name in workload["artifacts"]:
        relative = name.removeprefix("raw/")
        sources[f"artifacts/{relative}"] = bundle / name
    for name in ("SOURCE_INPUTS.json", "ENVIRONMENT.json", "COMMANDS.jsonl", "workloads.json",
                 "features.json", "ACCEPTANCE.json", "MANIFEST.json"):
        sources[f"evidence/{name}"] = bundle / name
    sources["evidence/verifier-controls.json"] = controls_path
    info = {
        "source": source, "local_qualification": "passed", "hosted_verification": "pending",
        "acceptance_manifest_sha256": file_hash(bundle / "MANIFEST.json"),
        "files": {name: file_hash(path) for name, path in sources.items()},
    }
    output = Path("build/release")
    output.mkdir(parents=True, exist_ok=True)
    destination = output / f"stratum-{source[:12]}.tar.gz"
    with tempfile.TemporaryDirectory(dir=output) as scratch:
        candidate = Path(scratch) / destination.name
        write_archive(candidate, sources, info)
        candidate.replace(destination)
    print(f"Local release package {destination} (SHA-256 {file_hash(destination)})")


if __name__ == "__main__":
    main()
