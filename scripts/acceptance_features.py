"""Map the frozen register to the source and tests that actually exercise it."""

import re


# Source paths point to the current implementation. An open item records the
# specific mechanism or proof still needed even when its related tests pass.
BINDINGS = {
    "K01": ("boot/uefi/main.c boot/uefi/elf.c", "boot"),
    "K02": ("kernel/core/boot_check.c", "boot production-fuzz"),
    "K03": ("kernel/arch/x86_64/entry.S kernel/core/boot.c", "boot"),
    "K04": ("kernel/arch/x86_64/idt.c kernel/arch/x86_64/interrupts.S", "diagnostics"),
    "K05": ("kernel/arch/x86_64/gdt.c kernel/arch/x86_64/smp.c", "boot threads"),
    "K06": ("kernel/drivers/acpi.c kernel/drivers/madt.c kernel/drivers/apic.c", "boot framebuffer block-timeout"),
    "K07": ("kernel/mm/pmm.c", "host shared"),
    "K08": ("kernel/mm/slab.c", "host diagnostics"),
    "K09": ("kernel/mm/vmm.c", "shared tlb"),
    "K10": ("kernel/mm/vmm.c kernel/mm/anonymous.c", "diagnostics shared"),
    "K11": ("kernel/mm/vmm.c kernel/mm/anonymous.c", "shared exec"),
    "K12": ("kernel/mm/vmm.c", "tlb wait-tlb-model"),
    "K13": ("kernel/mm/anonymous.c", "shared"),
    "K14": ("kernel/mm/vmm.c kernel/core/user_elf.c", "shared exec"),
    "K15": ("kernel/arch/x86_64/smp.c", "boot threads"),
    "K16": ("kernel/sync/spinlock.c", "diagnostics threads"),
    "K17": ("kernel/sync/mutex.c kernel/sync/event.c", "cancellation block-timeout wait-tlb-model"),
    "K18": ("kernel/sync/futex.c", "threads cancellation"),
    "K19": ("kernel/sched/sched.c", "threads"),
    "K20": ("kernel/sched/sched.c kernel/core/process.c", "threads"),
    "K21": ("kernel/sched/timer.c kernel/drivers/apic.c", "threads cancellation"),
    "K22": ("kernel/sched/sched.c kernel/net/net.c", "threads net-wrap soak"),
    "K23": ("kernel/core/user_elf.c kernel/core/exec.c", "exec production-fuzz"),
    "K24": ("kernel/core/process.c kernel/core/exec.c", "exec cancellation"),
    "K25": ("kernel/core/process.c user/lib/thread_start.S", "threads cancellation"),
    "K26": ("kernel/core/syscall.c kernel/arch/x86_64/syscall_asm.S", "threads exec"),
    "K27": ("kernel/core/file.c kernel/core/process.c", "exec ipc cancellation"),
    "K28": ("kernel/ipc/channel.c kernel/ipc/pipe.c", "ipc cancellation"),
    "K29": ("kernel/core/syscall.c kernel/sync/event.c", "ipc cancellation"),
    "K30": ("kernel/core/process.c kernel/sched/sched.c", "cancellation"),
    "K31": ("kernel/drivers/uart.c kernel/drivers/keyboard.c", "boot framebuffer regression"),
    "K32": ("kernel/drivers/fb.c kernel/drivers/keyboard.c", "framebuffer"),
    "K33": ("kernel/drivers/pci.c kernel/drivers/virtio.c", "block-timeout net-wrap"),
    "K34": ("kernel/drivers/virtqueue.c kernel/drivers/virtio.c", "block-wrap block-errors net-wrap"),
    "K35": ("kernel/drivers/virtio_blk.c", "block-wrap block-errors block-timeout storage-mixed"),
    "K36": ("kernel/drivers/virtio_net.c", "net-wrap net-reject tcp-reference"),
    "K37": ("kernel/fs/stratafs.c kernel/fs/journal.c", "storage-cache wal-crash block-errors"),
    "K38": ("kernel/fs/vfs.c", "storage-namespace tmpfs"),
    "K39": ("kernel/fs/cpio.c kernel/fs/initramfs.c kernel/fs/tmpfs.c", "exec tmpfs production-fuzz"),
    "K40": ("kernel/fs/stratafs.c tools/strata_check.c", "storage-mixed wal-crash"),
    "K41": ("kernel/fs/stratafs.c", "storage-mixed storage-namespace"),
    "K42": ("kernel/fs/journal.c", "wal-model wal-crash"),
    "K43": ("kernel/fs/journal.c kernel/fs/stratafs.c", "wal-crash service"),
    "K44": ("kernel/fs/stratafs.c tools/strata_check.c", "storage-cache storage-mixed storage-namespace"),
    "K45": ("kernel/net/ethernet.c kernel/net/arp.c kernel/net/ipv4.c kernel/net/icmp.c", "net-wrap net-reject"),
    "K46": ("kernel/net/udp.c kernel/net/socket.c", "net-wrap net-reject cancellation"),
    "K47": ("kernel/net/tcp.c kernel/net/socket.c", "tcp tcp-reference"),
    "K48": ("kernel/net/tcp.c", "tcp tcp-reference tcp-recovery"),
    "K49": ("kernel/net/net.c kernel/net/socket.c", "net-reject net-wrap soak"),
    "K50": ("user/lib/crt0.S user/lib/syscall.c include/user/strat_api.h", "exec threads service"),
    "K51": ("user/apps/init.c", "cancellation regression"),
    "K52": ("user/apps/sh.c user/apps/tools.c", "exec framebuffer trace"),
    "K53": ("user/apps/ledgerd.c user/apps/ledger_store.c", "service"),
    "K54": ("kernel/debug/trace.c include/shared/syscall_nums.h", "trace"),
    "K55": ("kernel/debug/panic.c", "diagnostics"),
    "K56": ("tools/strata_check.c", "storage-mixed storage-namespace wal-crash service"),
    "K57": ("tests/host/wal_model.py tests/host/wait_tlb_model.py", "host wal-model wait-tlb-model"),
    "K58": ("scripts/build_image.py scripts/build_subsystems.mk", "regression"),
    "K59": ("scripts/fuzz_production.py kernel/fs/stratafs.c", "production-fuzz storage-cache wal-crash"),
    "K60": ("tests/guest/soak_campaign.c scripts/test_soak.py", "soak"),
    "K61": ("scripts/test_guest_bench.py tests/guest/bench_campaign.c", "guest-bench"),
    "K62": ("README.md docs/BUILD_STATUS.md docs/ACCEPTANCE_PLAN.md", "documentation"),
    "K63": ("scripts/run_acceptance.py scripts/full_acceptance.py", "image regression"),
    "K64": ("scripts/verify_evidence.py scripts/verify_acceptance.py", "regression"),
}

OPEN = {
    "K05": "Dedicated nested interrupt and protected stack observations",
    "K08": "Kernel slab pressure with fault-injected rollback",
    "K21": "Deadline behavior while eligible tasks migrate",
    "K37": "Pinned, dirty block-cache entries and explicit flush-failure recovery",
    "K45": "Independent ICMP echo observations alongside ARP and IPv4",
    "K49": "Explicit socket exhaustion and capacity reuse",
    "K51": "Supervised native child restart",
    "K52": "Native process inspection command",
    "K54": "Per-event task identity and wait and wake observations",
    "K55": "Symbolized panic tied to exact build identity",
    "K61": "Equal-work comparisons against a frozen functional baseline",
    "K62": "Completed claim-to-evidence links and release walkthrough",
    "K63": "Complete fixed coverage on a published source and image",
    "K64": "Full accepted bundle and all independent controls",
}


def feature_rows(plan, results, commit):
    rows = {}
    for identity, title, _, expectation in re.findall(
        r"^\| (K\d\d) \| ([^|]+) \| ([^|]+) \| ([^|]+) \|$", plan, re.M
    ):
        paths, test_ids = BINDINGS[identity]
        observations = {}
        for name in test_ids.split():
            result = results[name]
            observations[name] = {
                "exit": result["exit"],
                "seconds": result.get("seconds"),
                "artifacts": sorted([result["log"], *result["artifacts"]]) if result["exit"] is not None else [],
            }
        statuses = [item["exit"] for item in observations.values()]
        if any(code not in (0, None) for code in statuses):
            status = "failed"
        elif None in statuses:
            status = "blocked"
        else:
            status = "unverified" if identity in OPEN else "passed"
        rows[identity] = {
            "source": commit, "feature": title.strip(), "source_paths": paths.split(),
            "expected": expectation.strip(), "observations": observations,
            "open_requirement": OPEN.get(identity), "status": status,
        }
    if rows.keys() != BINDINGS.keys() or len(rows) != 64:
        raise ValueError("capability register does not contain the 64 bound capability IDs")
    return rows
