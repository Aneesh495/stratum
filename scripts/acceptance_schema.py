"""Versioned commands and fixed gate coverage shared by evidence tools."""

SCHEMA = 3
# Keys correspond to docs/ACCEPTANCE_PLAN.md. A workload exit alone cannot clear a gap.
GATES = {
    "A01": ("boot", "framebuffer", "entry-stacks"),
    "A02": ("host", "diagnostics", "shared", "tlb", "resource-limits"),
    "A03": ("threads", "cancellation", "wait-tlb-model"),
    "A04": ("exec", "threads", "cancellation", "supervision"),
    "A05": ("ipc", "cancellation"),
    "A06": ("block-wrap", "block-errors", "block-timeout", "net-wrap"),
    "A07": ("wal-model", "storage-mixed", "storage-namespace", "storage-cache", "tmpfs", "wal-crash"),
    "A08": ("net-wrap", "tcp", "tcp-reference", "tcp-recovery", "net-reject", "icmp", "resource-limits"),
    "A09": ("service",),
    "A10": ("soak",),
    "A11": ("production-fuzz", "wait-tlb-model"),
    "A12": ("regression", "benchmarks", "guest-bench", "trace", "documentation"),
}
GAPS = {
    "A12": "Complete capability evidence, guest performance comparisons, and hosted release verification",
}


def python(script, *args):
    return ("python3", script, *args)


# Each tuple records a command, a hard timeout in seconds, and its raw outputs.
RUNS = {
    "image": (("make", "-j4", "image"), 240, ("images/*.img", "stratum.elf", "BOOTX64.EFI", "initramfs.cpio")),
    "boot": (python("scripts/test_boot_campaign.py"), 1200, ("boot-campaign/**/*",)),
    "framebuffer": (python("scripts/test_framebuffer.py"), 120, ("framebuffer/*",)),
    "entry-stacks": (python("scripts/test_entry_stacks.py"), 120, ("entry-stacks/*",)),
    "host": (("make", "test", "fuzz"), 300, ()),
    "diagnostics": (python("scripts/test_diagnostics.py", "all"), 300, ("diagnostics/*",)),
    "resource-limits": (python("scripts/test_resource_limits.py"), 120, ("resource-limits/*",)),
    "shared": (python("scripts/test_shared.py"), 300, ("shared/*",)),
    "tlb": (python("scripts/test_tlb.py"), 400, ("tlb/*",)),
    "threads": (python("scripts/test_threads.py"), 400, ("threads/*",)),
    "cancellation": (python("scripts/test_cancel.py"), 900, ("cancellation/*",)),
    "exec": (python("scripts/test_exec.py"), 400, ("exec/*",)),
    "supervision": (python("scripts/test_supervision.py"), 120, ("supervision/*",)),
    "ipc": (python("scripts/test_ipc.py"), 400, ("ipc/*",)),
    "block-wrap": (python("scripts/test_block_wrap.py"), 300, ("drivers/blk-wrap.log",)),
    "block-errors": (python("scripts/test_block_errors.py"), 720, ("drivers/blk-errors.log",)),
    "block-timeout": (python("scripts/test_block_timeout.py"), 180, ("block-timeout/*",)),
    "net-wrap": (python("scripts/test_net_wrap.py"), 900, ("net-wrap/*",)),
    "wal-model": (python("tests/host/wal_model.py"), 90, ()),
    "storage-mixed": (python("scripts/test_storage_mixed.py"), 420, ("storage-mixed/**/*",)),
    "storage-namespace": (python("scripts/test_storage_namespace.py"), 180, ("storage-namespace/*.log",)),
    "storage-cache": (python("scripts/test_storage_cache.py"), 120, ("storage-cache/*",)),
    "tmpfs": (python("scripts/test_tmpfs.py"), 120, ("tmpfs/*",)),
    "wal-crash": (python("scripts/test_wal_crash.py", "--repeats", "10"), 2100, ("wal-crash-*/**/*",)),
    "tcp": (python("scripts/test_tcp_campaign.py"), 1500, ("tcp-campaign-*/*",)),
    "tcp-recovery": (python("scripts/test_tcp_recovery.py"), 240, ("tcp-recovery/*",)),
    "tcp-reference": (python("scripts/test_tcp_reference.py"), 480, ("tcp-reference/*",)),
    "net-reject": (python("scripts/test_net_reject.py"), 240, ("net-reject-*/*",)),
    "icmp": (python("scripts/test_icmp.py"), 120, ("icmp/*",)),
    "service": (python("scripts/test_ledger_load.py"), 1800, ("ledger-load-*/*.log", "ledger-load-*/*.pcap",
                                                              "ledger-load-*/*.json")),
    "soak": (python("scripts/test_soak.py"), 3900, ("soak-*/*.log", "soak-*/*.pcap", "soak-*/*.json*")),
    "production-fuzz": (python("scripts/fuzz_production.py"), 800, ("production-fuzz-*/results.json",
                          "production-fuzz-*/*/*.log", "production-fuzz-*/*/seeds/*",
                          "production-fuzz-*/*/corpus/*")),
    "wait-tlb-model": (python("tests/host/wait_tlb_model.py"), 90, ()),
    "regression": (("make", "evidence"), 1800, ("evidence/20*/**/*",)),
    "benchmarks": (("make", "bench"), 300, ("host-benchmarks.log",)),
    "guest-bench": (python("scripts/test_guest_bench.py"), 900, ("guest-bench/*",)),
    "trace": (python("scripts/test_trace.py"), 120, ("trace/*",)),
    "documentation": (("make", "docs"), 90, ()),
}
