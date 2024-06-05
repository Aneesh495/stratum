"""Check a fixed-workload bundle without running workloads or editing evidence."""

from datetime import datetime
import json
import os
import re
from pathlib import Path
import subprocess

from acceptance_schema import GAPS, GATES, RUNS, SCHEMA
from evidence_format import file_hash


def require(condition, description):
    if not condition:
        raise ValueError(description)


def document(path):
    return json.loads(path.read_text(encoding="utf-8"))


def git(*args):
    return subprocess.check_output(["git", *args], timeout=30).rstrip(b"\n")


def verify(root):
    manifest = document(root / "MANIFEST.json")
    require(manifest["schema"] == SCHEMA, "unsupported acceptance manifest schema")
    require(not git("status", "--porcelain"), "current source tree is dirty")
    commit = git("rev-parse", "HEAD").decode()
    tree = git("rev-parse", "HEAD^{tree}").decode()
    require(manifest["source"] == commit, "acceptance source commit mismatch")
    inputs = document(root / "SOURCE_INPUTS.json")
    require(inputs["commit"] == commit and inputs["tree"] == tree, "source input identity mismatch")
    tracked = {os.fsdecode(path) for path in git("ls-files", "-z").split(b"\0") if path}
    require(inputs["files"].keys() == tracked, "source input list mismatch")
    for path in tracked:
        require(file_hash(Path(path)) == inputs["files"][path], f"source input hash mismatch: {path}")

    actual = {str(path.relative_to(root)) for path in root.rglob("*")
              if path.is_file() and path != root / "MANIFEST.json"}
    require(manifest["files"].keys() == actual, "missing or extra acceptance artifacts")
    for name in sorted(actual):
        path = root / name
        require(path.resolve().is_relative_to(root.resolve()) and not path.is_symlink(),
                f"unsafe acceptance artifact: {name}")
        require(file_hash(path) == manifest["files"][name], f"acceptance artifact digest mismatch: {name}")

    environment = document(root / "ENVIRONMENT.json")
    require(environment["system"] and environment["machine"] and environment["firmware_sha256"],
            "missing acceptance environment identity")
    workloads = document(root / "workloads.json")
    require(workloads.keys() == RUNS.keys(), "missing or extra acceptance workloads")
    history = [json.loads(row) for row in (root / "COMMANDS.jsonl").read_text().splitlines()]
    executed = [row["name"] for row in history]
    require(len(set(executed)) == len(executed), "duplicate command observations")
    require(set(executed) == {name for name, row in workloads.items() if row["exit"] is not None},
            "command observations disagree with workload results")
    for row in history:
        name = row["name"]
        command, timeout, patterns = RUNS[name]
        require(row == workloads[name] and row["command"] == list(command),
                f"{name}: command mismatch")
        require(type(row["exit"]) is int and row["timeout_seconds"] == timeout and
                row["timed_out"] == (row["exit"] == 124), f"{name}: exit or timeout mismatch")
        require(0 <= row["seconds"] <= timeout + 10 and
                datetime.fromisoformat(row["started"]) <= datetime.fromisoformat(row["ended"]),
                f"{name}: invalid duration")
        require(row["log"] == f"logs/{name}.log" and row["log"] in actual, f"{name}: missing raw command log")
        require(set(row["artifacts"]) <= actual and all(item.startswith("raw/") for item in row["artifacts"]),
                f"{name}: missing raw workload artifact")
        if not row["exit"] and patterns:
            require(row["artifacts"], f"{name}: completed workload has no observations")

    acceptance = document(root / "ACCEPTANCE.json")
    require(acceptance["schema"] == SCHEMA and acceptance["source"] == commit,
            "acceptance result identity mismatch")
    require(acceptance["gates"].keys() == GATES.keys(), "missing or extra fixed gates")
    features = document(root / "features.json")
    require(features.keys() == {f"K{i:02d}" for i in range(1, 65)}, "missing or extra capabilities")
    for gate, expected in GATES.items():
        row = acceptance["gates"][gate]
        failures = [name for name in expected if workloads[name]["exit"] not in (0, None)]
        blocked = [name for name in expected if workloads[name]["exit"] is None]
        status = "failed" if failures else ("blocked" if blocked else ("unverified" if gate in GAPS else "passed"))
        require(row["status"] == status and row["required_workloads"] == list(expected) and
                row["failed_workloads"] == failures and row["blocked_workloads"] == blocked and
                row["unverified_requirement"] == GAPS.get(gate), f"{gate}: inconsistent gate result")
    def result_path(workload, filename):
        matches = [root / path for path in workloads[workload]["artifacts"] if Path(path).name == filename]
        require(len(matches) == 1, f"{workload}: missing or ambiguous {filename}")
        return matches[0]

    if acceptance["gates"]["A01"]["status"] == "passed":
        boot_path = result_path("boot", "results.json")
        boot = document(boot_path)
        profiles = [(cpus, memory) for cpus in (1, 2, 4, 8) for memory in ("256M", "1G")] + [(1, "64M")]
        boots = {f"{cpus}cpu-{memory}-{repeat}" for cpus, memory in profiles for repeat in range(1, 6)}
        require(len(boot["fresh_boots"]) == len(boots) and set(boot["fresh_boots"]) == boots and
                len(boot["loader_rejections"]) == 17 and len(boot["handoff_rejections"]) == 111,
                "boot counts or matrix mismatch")
        for tag in boots:
            cpus = int(tag.split("cpu")[0])
            log = (boot_path.parent / f"{tag}.log").read_bytes()
            observed = re.findall(rb"^\[BOOT-PROBE\] cpu=(\d+) result=[0-9a-f]+\r?$", log, re.M)
            require(list(map(int, observed)) == list(range(cpus)), f"{tag}: missing CPU probe")
        require((boot_path.parent / "entry.json").is_file(), "missing debugger boot observation")

    if acceptance["gates"]["A11"]["status"] == "passed":
        campaign = result_path("production-fuzz", "results.json")
        report = document(campaign)
        require(report["source"] == commit and report["tree"] == tree and report["dirty"] is False and
                report["seconds_per_target"] == 600, "production fuzz source or duration mismatch")
        require(report["targets"].keys() == {"elf", "handoff", "cpio", "acpi", "stratafs", "ipv4"},
                "production fuzz target set mismatch")
        executions = 0
        for name, row in report["targets"].items():
            target = campaign.parent / name
            run_log = target / "run.log"
            observed = re.search(rb"stat::number_of_executed_units:\s+(\d+)", run_log.read_bytes())
            require(observed and int(observed[1]) == row["executions"] and row["executions"] > 0 and
                    row["elapsed_seconds"] >= 600 and row["covered_edges"] > 0,
                    f"{name}: invalid fuzz duration, coverage or execution count")
            require(file_hash(run_log) == row["log_sha256"] and
                    [file_hash(path) for path in sorted((target / "seeds").iterdir())] == row["seed_sha256"],
                    f"{name}: fuzz log or seed digest mismatch")
            executions += row["executions"]
        require(executions == report["total_executions"] and executions >= 2000000, "fuzz execution total mismatch")

    require(acceptance["result"] == "passed" and
            all(row["status"] == "passed" for row in acceptance["gates"].values()) and
            all(row["status"] == "passed" for row in features.values()),
            "bundle integrity checked, but required acceptance coverage remains incomplete")
