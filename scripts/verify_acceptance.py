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

    if acceptance["gates"]["A02"]["status"] == "passed":
        exhaustion = result_path("shared", "exhaust.log").read_bytes()
        require(b"allocator exhaustion, rollback, retry and final page recovery" in exhaustion,
                "missing real allocator exhaustion")
        require(b"2000000 / 2000000 ops completed" in (root / "logs/host.log").read_bytes(),
                "allocator model count mismatch")
        for cpu in (4, 8):
            shared = result_path("shared", f"{cpu}cpu.log").read_bytes()
            tlb = result_path("tlb", f"{cpu}cpu.log").read_bytes()
            require(b"shared mappings=2000 COW=2000" in shared and
                    f"cycles=10000 CPUs={cpu}".encode() in tlb, f"{cpu}: missing mapping evidence")
        demand = sum(b"12800 demand-zero allocations reconciled" in
                     result_path("diagnostics", f"stress-{cpu}.log").read_bytes()
                     for cpu in (1, 2, 4, 8))
        require(demand * 12800 >= 50000, "demand fault count below threshold")

    if acceptance["gates"]["A09"]["status"] == "passed":
        result = document(result_path("service", "results.json"))
        require(result["clients"] == 32 and result["logical_requests"] == 100000 and
                result["durable_records"] == 10000 and len(result["interruptions"]) == 10,
                "integrated service counts do not match the fixed workload")
        records = re.findall(rb"^\[RECORD\] entry(\d{4}) durable\r?$",
                             result_path("service", "follower.log").read_bytes(), re.M)
        require(len(records) == 10000 and set(map(int, records)) == set(range(10000)),
                "missing or duplicate durable service acknowledgments")
        for i, row in enumerate(result["interruptions"], 1):
            require(row["acknowledged_records"] > 0 and
                    b"ledgerd listening on port 9090." in result_path("service", f"primary-{i:02d}.log").read_bytes(),
                    f"missing service recovery {i}")
        for role in ("primary", "follower"):
            require(b"[PASSED]" in result_path("service", f"{role}-check.log").read_bytes(),
                    f"missing independent {role} disk check")
        require(file_hash(result_path("service", "ethernet.pcap")) == result["pcap_sha256"],
                "integrated service packet capture mismatch")

    if acceptance["gates"]["A10"]["status"] == "passed":
        result = document(result_path("soak", "results.json"))
        rounds = [json.loads(row) for row in result_path("soak", "rounds.jsonl").read_text().splitlines()]
        require(result["cpus"] == 8 and result["memory"] == "1G" and
                result["requested_seconds"] == 3600 and len(rounds) == result["rounds"] and
                result["elapsed_seconds"] >= 3600, "soak profile, count or duration mismatch")
        baseline = result["baseline"]
        observed = ("processes", "free_kib", "threads", "files", "packets", "journal_capacity", "journal_pending")
        elapsed, uptime, syscalls = 0, baseline["uptime_ms"], baseline["syscalls"]
        for index, row in enumerate(rounds, 1):
            require(row["round"] == index and row["elapsed_seconds"] > elapsed and
                    row["uptime_ms"] > uptime and row["syscalls"] > syscalls and
                    all(row[name] == baseline[name] for name in observed),
                    f"soak round {index}: stalled workload or resource mismatch")
            elapsed, uptime, syscalls = row["elapsed_seconds"], row["uptime_ms"], row["syscalls"]
        require(elapsed >= 3600 and baseline["packets"] == baseline["journal_pending"] == 0 and
                baseline["journal_capacity"] > 0, "soak did not quiesce")
        require(file_hash(result_path("soak", "ethernet.pcap")) == result["pcap_sha256"] and
                b"[PASSED]" in result_path("soak", "disk.log").read_bytes(), "missing soak wire or disk evidence")

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
