#!/usr/bin/env python3
"""Generate fresh regression evidence from two isolated copies of committed source."""

import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import tempfile
import time

from evidence_format import ARTIFACTS, COMMANDS, REQUIRED_TRANSCRIPTS, SCHEMA, file_hash


def git(*args):
    return subprocess.check_output(["git", *args], text=True, timeout=30).strip()


def run(name, command, cwd, destination):
    print(f"Running {name}...", flush=True)
    start = time.monotonic()
    logfile = destination / "logs" / f"{name}.log"
    with logfile.open("w", encoding="utf-8") as log:
        try:
            result = subprocess.run(command, cwd=cwd, stdout=log, stderr=subprocess.STDOUT,
                                    timeout=600)
            status = result.returncode
        except subprocess.TimeoutExpired:
            log.write("\nProcess timed out.\n")
            status = 124
    if status:
        print(logfile.read_text(encoding="utf-8", errors="replace")[-3000:], file=sys.stderr)
    return {"command": list(command), "exit": status, "seconds": round(time.monotonic() - start, 3)}


def copy_outputs(source, dest, files):
    for name in files:
        origin = source / name
        if not origin.is_file():
            continue  # Missing evidence is rejected by the verifier.
        target = dest / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(origin, target)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--smoke", action="store_true", help="generate the supported regression bundle")
    args = parser.parse_args()
    if not args.smoke:
        print("Full acceptance is unverified. See docs/BUILD_STATUS.md for open gates.", file=sys.stderr)
        print("Run make evidence to generate and verify the supported regression bundle.", file=sys.stderr)
        return 2

    if git("status", "--porcelain"):
        parser.error("commit or set aside working tree changes before generating evidence")
    source = git("rev-parse", "HEAD")
    tree = git("rev-parse", "HEAD^{tree}")
    timestamp = datetime.now(timezone.utc)
    root = Path("build/evidence")
    root.mkdir(parents=True, exist_ok=True)
    dest = Path(tempfile.mkdtemp(prefix=timestamp.strftime("%Y%m%dT%H%M%SZ-"), dir=root)).resolve()
    (dest / "logs").mkdir()
    results = {}

    archive = subprocess.check_output(["git", "archive", source], timeout=30)
    with tempfile.TemporaryDirectory(prefix="sources-", dir=root) as directory:
        a, b = (Path(directory) / label for label in ("build-a", "build-b"))
        for checkout in (a, b):
            checkout.mkdir()
            subprocess.run(["tar", "-xf", "-", "-C", checkout], input=archive, check=True, timeout=30)
        for name, command in COMMANDS.items():
            checkout = b if name == "build-b" else a
            results[name] = run(name, command, checkout, dest)

        for label, checkout in (("build-a", a), ("build-b", b)):
            copy_outputs(checkout / "build", dest / "artifacts" / label, ARTIFACTS)
        copy_outputs(a / "build", dest / "transcripts", REQUIRED_TRANSCRIPTS)

    manifest = {
        "schema": SCHEMA,
        "kind": "regression",
        "source_commit": source,
        "source_tree": tree,
        "created_at": timestamp.isoformat(),
        "host": {"system": platform.system(), "machine": platform.machine()},
        "full_acceptance": "unverified",
        "commands": results,
        "files": {str(path.relative_to(dest)): file_hash(path)
                  for path in sorted(dest.rglob("*")) if path.is_file()},
    }
    (dest / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(f"Regression evidence: {dest.relative_to(Path.cwd())}", flush=True)
    result = subprocess.run([sys.executable, "scripts/verify_evidence.py", str(dest)], timeout=120)
    if result.returncode:
        return result.returncode
    return subprocess.run([sys.executable, "scripts/test_evidence.py", str(dest)], timeout=120).returncode


if __name__ == "__main__":
    sys.exit(main())
