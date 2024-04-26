"""Paths and commands in the versioned regression evidence format."""

from hashlib import sha256


SCHEMA = 4
ARTIFACTS = (
    "BOOTX64.EFI", "stratum.elf", "initramfs.cpio", "user_init.elf", "user_test.elf",
    "user_tools.elf", "user_exec_campaign.elf", "user_thread_campaign.elf",
    "images/stratum-boot.img", "images/stratum-data.img",
)
COMMANDS = {
    "doctor": ("make", "doctor", "docs"),
    "host": ("make", "test", "fuzz"),
    "build-a": ("make", "-j2", "image"),
    "build-b": ("make", "-j2", "image"),
    "boot": ("python3", "scripts/test_boot.py"),
    "exec": ("python3", "scripts/test_exec.py"),
    "wait-tlb-model": ("python3", "tests/host/wait_tlb_model.py"),
    "tlb": ("python3", "scripts/test_tlb.py"),
    "threads": ("python3", "scripts/test_threads.py"),
    "matrix": ("python3", "scripts/test_smoke.py", "--matrix"),
    "diagnostics": ("python3", "scripts/test_diagnostics.py", "all"),
    "drivers": ("python3", "scripts/test_block_wrap.py"),
    "recovery": ("python3", "scripts/test_recovery.py"),
    "ledger": ("python3", "scripts/test_ledger.py"),
    "replication": ("python3", "scripts/test_replication.py"),
}
PROFILES = (
    [(n, mem) for mem in ("256M", "1G") for n in (1, 2, 4, 8)] + [(1, "64M")]
)
REQUIRED_TRANSCRIPTS = (
    ["boot/no-disk.log", "boot/phoff-overflow.log", "boot/file-larger-than-memory.log"]
    + [f"smoke/{n}cpu-{mem}-{stage}.log" for n, mem in PROFILES for stage in ("boot", "reboot")]
    + [f"{suite}/{n}cpu.log" for suite in ("exec", "threads", "tlb") for n in (4, 8)]
    + [f"diagnostics/stress-{n}.log" for n in (1, 2, 4, 8)]
    + ["drivers/blk-wrap.log", "diagnostics/faults-2.log", "recovery/setup.log", "recovery/interrupted.log",
       "recovery/rejected.log", "recovery/replayed.log", "recovery/bad-superblock.log",
       "ledger/before-stop.log", "ledger/after-restart.log", "replication/primary.log",
       "replication/follower.log", "replication/follower-reboot.log", "replication/ethernet.pcap"]
)


def file_hash(path):
    with path.open("rb") as data:
        h = sha256()
        while chunk := data.read(65536):
            h.update(chunk)
        return h.hexdigest()
