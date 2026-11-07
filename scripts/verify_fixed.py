"""Raw observation checks for concurrency, device, filesystem and wire campaigns."""

import re

from test_ipc import check_transcript
from test_tcp_campaign import BULK_BYTES, IMPAIRED_BYTES, verify_wire
from test_threads import check_population
from evidence_format import file_hash


def verify(root, gates, result_path, document, require):
    def completed(gate):
        return gates[gate]["status"] == "passed"

    def raw(workload, filename):
        return result_path(workload, filename).read_bytes()

    def check(workload, filename, *markers):
        content = raw(workload, filename)
        for marker in markers:
            require(marker in content, f"{workload}/{filename}: missing {marker!r}")

    if completed("A01"):
        from test_framebuffer import contains_glyph_row

        require(not contains_glyph_row(result_path("framebuffer", "before.ppm")) and
                contains_glyph_row(result_path("framebuffer", "after.ppm")), "framebuffer glyph mismatch")
        check("framebuffer", "guest.log", b"sysinfo", b"ZZZZZZZZ")

    for cpus in (4, 8):
        name = f"{cpus}cpu.log"
        if completed("A03"):
            check_population(raw("threads", name), cpus)
            check("threads", name, b"futex cases=10000 mismatch=2500 wake=2500 timeout=2500 unmap=2500",
                  b"Every runnable worker completed within 60000 guest milliseconds")
        if completed("A04"):
            check("exec", name, b"fork/exec/wait cycles=10000 with argv, envp, CLOEXEC and resource recovery")
            check("threads", name, b"thread create/join cycles=25000", b"syscall boundaries=10000")
        if completed("A05"):
            check_transcript(raw("ipc", name), "bb15c4cb1fd3582d", "f94679431f16ead7")
        if any(completed(gate) for gate in ("A03", "A04", "A05")):
            check("cancellation", name, f"cancellations=10008 group_exits=256 CPUs={cpus}".encode(),
                  b"Socket poll races=2500 and orphan process reaps=32")

    if completed("A06"):
        check("block-wrap", "blk-wrap.log", b"150000 reads, 2 available wraps, 2 used wraps")
        check("block-errors", "blk-errors.log", b"Block saturation: 500 cases", b"Block reset: 500 cases",
              b"Block I/O errors: 1000 device failures")
        for cpus in (2, 4):
            check("block-timeout", f"{cpus}cpu.log", b"MSI-X completion vector", b"Request timed out",
                  b"bounded read error", b"reset restored exact fixture bytes")

    if completed("A07"):
        require(b"validated 5000 crash states and rejected 1500 corrupt states" in
                (root / "logs/wal-model.log").read_bytes(), "WAL model count mismatch")
        require(b"128006 mixed filesystem operations" in (root / "logs/storage-mixed.log").read_bytes(),
                "mixed filesystem count mismatch")
        check("storage-cache", "cache.log", b"StrataFS cache hits=", b"Block MSI-X woke")
        check("storage-cache", "alloc-fail.log", b"occurrence=2", b"allocation rollback and exact retry bytes")
        check("tmpfs", "initial.log", b"tmpfs mount, sparse I/O, namespace and retained handles")
        check("tmpfs", "reboot.log", b"err=-2", b"disk")
        campaign = result_path("wal-crash", "campaign.json")
        report = document(campaign)
        from test_wal_crash import STAGES

        stages = dict(STAGES)
        actual = {(row["stage"], row["repetition"]) for row in report["cases"]}
        require(report["completed"] == len(report["cases"]) == 100 and report["stages"] == 10 and
                actual == {(stage, i) for stage in stages for i in range(1, 11)}, "WAL case matrix mismatch")
        for row in report["cases"]:
            stage, repetition = row["stage"], row["repetition"]
            identity = f"{repetition:02d}-{stage}"
            case = campaign.parent / identity
            expected = ["baseline"]
            if stages[stage]:
                expected.append(f"run-{identity}")
            log = (case / "reboot.log").read_bytes()
            require(row["payload"] == expected and document(case / "result.json") == row and
                    b"\r\n".join(x.encode() for x in expected) + b"\r\n" in log and
                    bool(b"Replayed committed transaction" in log) == row["replayed"] ==
                    (stages[stage] and stage != "checkpointed") and
                    f"WAL crash stage {stage}.".encode() in (case / "interrupted.log").read_bytes() and
                    b"[PASSED]" in (case / "checker.log").read_bytes(), f"{identity}: WAL recovery mismatch")

    if completed("A06") or completed("A08"):
        udp = document(result_path("net-wrap", "results.json"))
        require(udp["datagrams_per_direction"] == 150000 and
                [row[:5] for row in udp["queues"]] == [
                    ["rx", 150000, 150000, 2, 2], ["tx", 150000, 150000, 2, 2]
                ], "UDP datagram or queue-wrap count mismatch")
        require(file_hash(result_path("net-wrap", "ethernet.pcap")) == udp["pcap_sha256"],
                "UDP capture identity mismatch")

    if completed("A08"):
        report = document(result_path("tcp", "results.json"))["results"]
        for role, sessions, size in (("host", 800, BULK_BYTES), ("impaired", 200, IMPAIRED_BYTES)):
            observed = report[role]
            path = result_path("tcp", f"{role}-ethernet.pcap")
            require(observed["sessions"] == sessions and observed["bytes"] == sessions * size * 2 and
                    file_hash(path) == observed["pcap_sha256"] and
                    verify_wire(path, sessions, size) == observed["wire_payload_sha256"],
                    f"{role}: TCP stream evidence mismatch")
        impairments = report["impaired"]["impairments"]
        require(len(impairments) == 200 and {kind for kind, _ in impairments.values()} ==
                {"loss", "duplicate", "reorder"}, "TCP impairment count or coverage mismatch")
        from test_tcp_reference import ISO_SHA256

        reference = document(result_path("tcp-reference", "results.json"))
        capture = result_path("tcp-reference", "ethernet.pcap")
        require(reference["iso_sha256"] == ISO_SHA256 and reference["sessions"] == 32 and
                reference["bytes"] == 4194304 and file_hash(capture) == reference["pcap_sha256"] and
                verify_wire(capture, 32, 65536) == reference["wire_payload_sha256"],
                "direct Linux TCP reference evidence mismatch")
        recovery = document(result_path("tcp-recovery", "results.json"))
        recovery_wire = result_path("tcp-recovery", "impaired-ethernet.pcap")
        require(recovery["sessions"] == 24 and recovery["bytes"] == 1572864 and
                recovery["fast_retransmits"] > 0 and len(recovery["impairments"]) == 24 and
                file_hash(recovery_wire) == recovery["pcap_sha256"] and
                verify_wire(recovery_wire, 24, 32768) == recovery["wire_payload_sha256"],
                "fast TCP recovery or reassembly evidence mismatch")
        check("tcp-recovery", "impaired-server.log",
              f"fast_retransmits={recovery['fast_retransmits']}".encode())
        malformed = document(result_path("net-reject", "results.json"))
        require(malformed["rejected"] == 10000 and malformed["valid_echoes"] == 2000 and
                len(malformed["cases"]) == 10 and set(malformed["cases"].values()) == {1000} and
                file_hash(result_path("net-reject", "ethernet.pcap")) == malformed["pcap_sha256"],
                "invalid Ethernet case or wire evidence mismatch")
