"""Versioned commands and fixed gate coverage shared by evidence tools."""

SCHEMA = 1
# Keys correspond to docs/ACCEPTANCE_PLAN.md. A workload exit alone cannot clear a gap.
GATES = {
    "A01": ("boot",),
    "A02": ("host", "diagnostics", "shared", "tlb"),
    "A03": ("threads", "wait-tlb-model"),
    "A04": ("exec", "threads"),
    "A05": ("ipc",),
    "A06": ("block-wrap", "block-errors", "net-wrap"),
    "A07": ("wal-model", "storage-mixed", "storage-namespace", "wal-crash"),
    "A08": ("net-wrap", "tcp", "net-reject"),
    "A09": ("service",),
    "A10": ("soak",),
    "A11": ("production-fuzz", "wait-tlb-model"),
    "A12": ("regression", "benchmarks", "documentation"),
}
GAPS = {
    "A02": "Real allocator exhaustion and failure rollback coverage",
    "A03": "Process-wide cancellation and bounded scheduler fairness checks",
    "A04": "Process-wide cancellation and broader boundary cases",
    "A05": "Cancellation races and broader socket readiness checks",
    "A06": "Interrupt-driven block completion and device timeout recovery under load",
    "A07": "Block-cache pressure and storage allocation failure injection",
    "A08": "Direct independent TCP reference on the Ethernet link",
    "A09": "Durable service checkpoint replacement",
    "A10": "Independent quiescent task, buffer, and journal reconciliation",
    "A12": "Complete capability evidence, independent negative controls, and hosted release verification",
}


def python(script, *args):
    return ("python3", script, *args)


# Each tuple records a command, a hard timeout in seconds, and its raw outputs.
RUNS = {
    "image": (("make", "-j4", "image"), 240, ("images/*.img", "stratum.elf", "BOOTX64.EFI", "initramfs.cpio")),
    "boot": (python("scripts/test_boot_campaign.py"), 1200, ("boot-campaign/**/*",)),
    "host": (("make", "test", "fuzz"), 300, ()),
    "diagnostics": (python("scripts/test_diagnostics.py", "all"), 300, ("diagnostics/*",)),
    "shared": (python("scripts/test_shared.py"), 300, ("shared/*",)),
    "tlb": (python("scripts/test_tlb.py"), 400, ("tlb/*",)),
    "threads": (python("scripts/test_threads.py"), 400, ("threads/*",)),
    "exec": (python("scripts/test_exec.py"), 400, ("exec/*",)),
    "ipc": (python("scripts/test_ipc.py"), 400, ("ipc/*",)),
    "block-wrap": (python("scripts/test_block_wrap.py"), 300, ("drivers/blk-wrap.log",)),
    "block-errors": (python("scripts/test_block_errors.py"), 720, ("drivers/blk-errors.log",)),
    "net-wrap": (python("scripts/test_net_wrap.py"), 900, ("net-wrap/*",)),
    "wal-model": (python("tests/host/wal_model.py"), 90, ()),
    "storage-mixed": (python("scripts/test_storage_mixed.py"), 420, ("storage-mixed/**/*",)),
    "storage-namespace": (python("scripts/test_storage_namespace.py"), 180, ("storage-namespace/*.log",)),
    "wal-crash": (python("scripts/test_wal_crash.py", "--repeats", "10"), 2100, ("wal-crash-*/**/*",)),
    "tcp": (python("scripts/test_tcp_campaign.py"), 1500, ("tcp-campaign-*/*",)),
    "net-reject": (python("scripts/test_net_reject.py"), 240, ("net-reject-*/*",)),
    "service": (python("scripts/test_ledger_load.py"), 1800, ("ledger-load-*/*.log", "ledger-load-*/*.pcap",
                                                              "ledger-load-*/*.json")),
    "soak": (python("scripts/test_soak.py"), 3900, ("soak-*/*.log", "soak-*/*.pcap", "soak-*/*.json*")),
    "production-fuzz": (python("scripts/fuzz_production.py"), 800, ("production-fuzz-*/results.json",
                          "production-fuzz-*/*/*.log", "production-fuzz-*/*/seeds/*",
                          "production-fuzz-*/*/corpus/*")),
    "wait-tlb-model": (python("tests/host/wait_tlb_model.py"), 90, ()),
    "regression": (("make", "evidence"), 1800, ("evidence/20*/**/*",)),
    "benchmarks": (("make", "bench"), 300, ("host-benchmarks.log",)),
    "documentation": (("make", "docs"), 90, ()),
}
