"""Run the fixed Stratum workloads from a clean copy and retain partial evidence."""

from datetime import datetime, timezone
import json
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time

from acceptance_schema import GAPS, GATES, RUNS, SCHEMA
from evidence_format import file_hash
from vm import firmware, tool


def utc():
    return datetime.now(timezone.utc).isoformat()


def write(path, content):
    path.write_text(json.dumps(content, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def git(root, *args):
    return subprocess.check_output(["git", "-C", str(root), *args], timeout=30).rstrip(b"\n")


def source_files(root):
    files = git(root, "ls-files", "-z").rstrip(b"\0").split(b"\0")
    return {os.fsdecode(path): file_hash(root / os.fsdecode(path)) for path in files if path}


def environment():
    versions = {}
    for name in ("clang", "ld.lld", "qemu-system-x86_64", "nasm", "mcopy"):
        executable = tool(name)
        try:
            versions[name] = subprocess.check_output([executable, "--version"], text=True,
                                                     stderr=subprocess.STDOUT, timeout=10).splitlines()[0]
        except (OSError, IndexError, subprocess.SubprocessError):
            versions[name] = None
    rom = Path(firmware())
    return {"tool_versions": versions, "firmware_sha256": file_hash(rom) if rom.is_file() else None,
            "python": sys.version.split()[0], "machine": os.uname().machine, "system": os.uname().sysname}


def execute(name, checkout, dest, command, timeout):
    path = dest / "logs" / f"{name}.log"
    row = {"name": name, "command": command, "timeout_seconds": timeout, "started": utc(),
           "log": str(path.relative_to(dest)), "timed_out": False}
    start = time.monotonic()
    print(f"Running {name}...", flush=True)
    with path.open("wb") as log:
        try:
            with subprocess.Popen(command, cwd=checkout, stdout=log, stderr=subprocess.STDOUT,
                                  start_new_session=True) as process:
                try:
                    row["exit"] = process.wait(timeout=timeout)
                except subprocess.TimeoutExpired:
                    row["timed_out"] = True
                    os.killpg(process.pid, signal.SIGTERM)
                    try:
                        process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        os.killpg(process.pid, signal.SIGKILL)
                        process.wait()
                    row["exit"] = 124
        except OSError as error:
            log.write((str(error) + "\n").encode())
            row["exit"] = 127
    row.update({"seconds": round(time.monotonic() - start, 3), "ended": utc()})
    if row["exit"]:
        print(f"[FAIL] {name} exited {row['exit']}. See {path}", flush=True)
    return row


def preserve(checkout, dest, patterns):
    source = checkout / "build"
    kept = []
    for original in sorted({p for pattern in patterns for p in source.glob(pattern)}):
        if not original.is_file() or original.is_symlink():
            continue
        target = dest / "raw" / original.relative_to(source)
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(original, target)
        kept.append(str(target.relative_to(dest)))
    return kept


def run():
    root = Path.cwd()
    if git(root, "status", "--porcelain"):
        raise RuntimeError("commit the source before generating full evidence")
    commit = git(root, "rev-parse", "HEAD").decode()
    tree = git(root, "rev-parse", "HEAD^{tree}").decode()
    output = root / "build" / "acceptance"
    output.mkdir(parents=True, exist_ok=True)
    dest = Path(tempfile.mkdtemp(prefix="run-", dir=output))
    (dest / "logs").mkdir()
    print(f"Acceptance evidence: {dest}", flush=True)
    results = {}
    with tempfile.TemporaryDirectory(prefix="frozen-", dir=output) as workspace:
        checkout = Path(workspace) / "source"
        subprocess.run(["git", "clone", "--quiet", "--local", "--no-hardlinks", str(root), str(checkout)],
                       check=True, timeout=120)
        subprocess.run(["git", "-C", str(checkout), "checkout", "--quiet", "--detach", commit],
                       check=True, timeout=30)
        write(dest / "SOURCE_INPUTS.json", {"commit": commit, "tree": tree, "files": source_files(checkout)})
        write(dest / "ENVIRONMENT.json", environment())
        with (dest / "COMMANDS.jsonl").open("w", encoding="utf-8") as history:
            for name, (command, timeout, patterns) in RUNS.items():
                if name not in ("image", "host", "documentation", "benchmarks", "wait-tlb-model", "wal-model",
                                "production-fuzz", "regression") and results["image"]["exit"]:
                    results[name] = {"name": name, "blocked_by": "image", "exit": None, "artifacts": []}
                    continue
                row = execute(name, checkout, dest, command, timeout)
                row["artifacts"] = preserve(checkout, dest, patterns)
                results[name] = row
                history.write(json.dumps(row) + "\n")
                history.flush()

    gates = {}
    for gate, requirements in GATES.items():
        failed = [name for name in requirements if results[name]["exit"] not in (None, 0)]
        blocked = [name for name in requirements if results[name]["exit"] is None]
        gap = GAPS.get(gate)
        status = "failed" if failed else ("blocked" if blocked else ("unverified" if gap else "passed"))
        gates[gate] = {"status": status, "failed_workloads": failed, "blocked_workloads": blocked,
                       "required_workloads": requirements, "unverified_requirement": gap}
    features = {}
    plan = root.joinpath("docs/ACCEPTANCE_PLAN.md").read_text()
    for identity, title, paths, verification in re.findall(r"^\| (K\d\d) \| ([^|]+) \| ([^|]+) \| ([^|]+) \|$", plan, re.M):
        features[identity] = {"feature": title, "declared_paths": paths, "verification": verification,
                              "status": "unverified"}
    if len(features) != 64:
        raise RuntimeError("capability register does not contain exactly 64 rows")
    write(dest / "features.json", features)
    write(dest / "workloads.json", results)
    write(dest / "ACCEPTANCE.json", {"schema": SCHEMA, "source": commit, "result": "incomplete", "gates": gates})
    write(dest / "MANIFEST.json", {"schema": SCHEMA, "source": commit, "files": {
        str(path.relative_to(dest)): file_hash(path) for path in sorted(dest.rglob("*")) if path.is_file()
    }})
    print("Full acceptance is incomplete. See ACCEPTANCE.json for failed and unverified gates.", flush=True)
    return 1
