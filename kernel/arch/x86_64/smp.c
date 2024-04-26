#include <kernel/smp.h>
#include <kernel/pmm.h>
#include <kernel/vmm.h>
#include <kernel/idt.h>
#include <kernel/gdt.h>
#include <kernel/apic.h>
#include <kernel/acpi.h>
#include <kernel/kernel.h>
#include <kernel/sched.h>
#include <kernel/syscall.h>
#include <kernel/string.h>
#include <kernel/x86_64.h>
#include <shared/errno.h>

extern const uint8_t g_trampoline_binary[];
extern const uint64_t g_trampoline_binary_len;

cpu_t g_cpus[SMP_MAX_CPUS];
volatile uint32_t g_smp_online_cpus = 0;
static spinlock_t g_smp_boot_lock = SPINLOCK_INIT;
static spinlock_t shootdown_lock = SPINLOCK_INIT;
static bool shootdown_failed;
static uint64_t shootdown_sequence;

cpu_t *smp_get_current_cpu(void) {
    uint64_t gs = rdmsr(MSR_GS_BASE);
    if (gs == 0) {
        return &g_cpus[0];
    }
    return (cpu_t *)gs;
}

uint32_t smp_get_cpu_id(void) {
    return smp_get_current_cpu()->cpu_id;
}

uint32_t smp_get_online_cpus(void) {
    return __atomic_load_n(&g_smp_online_cpus, __ATOMIC_ACQUIRE);
}

static void smp_handle_tlb_shootdown(interrupt_frame_t *frame) {
    (void)frame;
    cpu_t *me = smp_get_current_cpu();
    uint64_t request = __atomic_load_n(&me->tlb_shootdown_request, __ATOMIC_ACQUIRE);
    if (request == __atomic_load_n(&me->tlb_shootdown_ack, __ATOMIC_RELAXED)) return;
    uint64_t va = __atomic_load_n(&me->tlb_shootdown_addr, __ATOMIC_RELAXED);
    if (va) invlpg(va);
    else write_cr3(read_cr3());
    __atomic_store_n(&me->tlb_shootdown_ack, request, __ATOMIC_RELEASE);
}

static void smp_handle_halt(interrupt_frame_t *frame) {
    (void)frame;
    cli();
    while (1) {
        hlt();
    }
}

static void smp_handle_spurious(interrupt_frame_t *frame) {
    (void)frame;
    /* Spurious interrupt requires no action or EOI */
}

static void smp_handle_resched(interrupt_frame_t *frame) {
    (void)frame;
    sched_reschedule();
}

void smp_send_ipi(uint32_t dest_apic_id, uint8_t vector) {
    lapic_send_ipi(dest_apic_id, ICR_NO_SHORTHAND | ICR_PHYSICAL | ICR_ASSERT | ICR_EDGE, vector);
}

void smp_broadcast_ipi(uint8_t vector, bool include_self) {
    uint32_t shorthand = include_self ? ICR_ALL_INCL_SELF : ICR_ALL_EXCL_SELF;
    lapic_send_ipi(0, shorthand | ICR_PHYSICAL | ICR_ASSERT | ICR_EDGE, vector);
}

void smp_halt_all(void) {
    smp_broadcast_ipi(VEC_IPI_HALT, false);
}

int smp_tlb_shootdown(uint64_t vaddr) {
    uint64_t flags = local_irq_save();
    if (!spin_trylock(&shootdown_lock)) {
        local_irq_restore(flags);
        return -STRATUM_EBUSY;
    }
    int result = 0;
    if (shootdown_failed || shootdown_sequence == (uint64_t)-1) {
        result = -STRATUM_EIO;
        goto out;
    }
    if (vaddr) invlpg(vaddr);
    else write_cr3(read_cr3());

    /* Publish one generation to the fixed online CPU set. The caller keeps
       the retired mapping's backing alive until this generation is complete. */
    uint64_t request = ++shootdown_sequence;
    uint32_t current = smp_get_cpu_id();
    uint32_t online = smp_get_online_cpus();
    for (uint32_t cpu = 0; cpu < online; cpu++) {
        if (cpu == current) continue;
        __atomic_store_n(&g_cpus[cpu].tlb_shootdown_addr, vaddr, __ATOMIC_RELAXED);
        __atomic_store_n(&g_cpus[cpu].tlb_shootdown_request, request, __ATOMIC_RELEASE);
        smp_send_ipi(g_cpus[cpu].lapic_id, VEC_IPI_TLB);
    }
    for (uint32_t cpu = 0; cpu < online; cpu++) {
        if (cpu == current) continue;
        unsigned spins = 25000000;
        while (__atomic_load_n(&g_cpus[cpu].tlb_shootdown_ack, __ATOMIC_ACQUIRE) != request) {
            if (spins-- == 0) {
                shootdown_failed = true;
                result = -STRATUM_ETIMEDOUT;
                goto out;
            }
            cpu_pause();
        }
    }
out:
    spin_unlock(&shootdown_lock);
    local_irq_restore(flags);
    return result;
}

void smp_ap_entry(uint64_t cpu_id) {
    cpu_t *cpu = &g_cpus[cpu_id];

    /* 1. Setup per-CPU GDT and TSS */
    gdt_init_ap(cpu);

    /* 2. Load IDT */
    idt_load();

    /* 3. Setup GS Base MSR to point to this cpu_t */
    wrmsr(MSR_GS_BASE, (uint64_t)cpu);

    /* 4. Initialize Local APIC on this AP */
    lapic_init();

    /* Initialize Fast Syscall MSRs on AP */
    syscall_init_cpu();

    /* Initialize per-CPU scheduler runqueue */
    sched_init_cpu((uint32_t)cpu_id);

    /* Initialize Local APIC periodic timer on this AP */
    lapic_timer_init_ap();

    /* 5. Mark CPU online and update global count */
    cpu->is_online = true;
    __atomic_add_fetch(&g_smp_online_cpus, 1, __ATOMIC_SEQ_CST);

    /* 6. Signal mailbox to unblock BSP */
    smp_mailbox_t *mb = (smp_mailbox_t *)phys_to_virt(SMP_MAILBOX_PHYS);
    mb->status = 1;

    kprintf("[SMP] CPU #%lu online and active (APIC ID %u)\n",
            cpu_id, lapic_get_id());

    /* 7. Enable interrupts on this AP */
    sti();

    /* 8. Enter AP idle loop */
    while (1) {
        hlt();
    }
}

void smp_init(const boot_handoff_t *handoff) {
    (void)handoff;
    memset(g_cpus, 0, sizeof(g_cpus));
    g_smp_online_cpus = 0;

    /* 1. Initialize ACPI to discover topology */
    if (acpi_init(handoff) != 0) {
        memset(&g_acpi_info, 0, sizeof(g_acpi_info));
        kprintf("[SMP] ACPI initialization failed, running in uniprocessor mode.\n");
    }

    /* 2. Initialize BSP Local APIC */
    lapic_init();

    /* 3. Configure BSP CPU structure */
    g_cpus[0].cpu_id = 0;
    g_cpus[0].lapic_id = lapic_get_id();
    g_cpus[0].is_bsp = true;
    g_cpus[0].is_online = true;
    wrmsr(MSR_GS_BASE, (uint64_t)&g_cpus[0]);
    g_smp_online_cpus = 1;

    /* 4. Register IPI interrupt vectors in IDT */
    register_interrupt_handler(VEC_IPI_TLB, smp_handle_tlb_shootdown);
    register_interrupt_handler(VEC_IPI_HALT, smp_handle_halt);
    register_interrupt_handler(VEC_IPI_RESCHED, smp_handle_resched);
    register_interrupt_handler(VEC_SPURIOUS, smp_handle_spurious);

    /* 5. Initialize I/O APIC */
    ioapic_init();

    /* 6. Calibrate and initialize BSP Local APIC Timer (100 Hz) */
    lapic_timer_init(100);

    /* Check if multi-core topology exists */
    if (g_acpi_info.cpu_count <= 1) {
        kprintf("[SMP] Single processor detected. Running with 1 CPU.\n");
        return;
    }

    /* 6. Copy AP trampoline binary to physical address 0x8000 (< 1 MiB) */
    void *tramp_dst = phys_to_virt(SMP_TRAMPOLINE_PHYS);
    memcpy(tramp_dst, g_trampoline_binary, g_trampoline_binary_len);

    smp_mailbox_t *mb = (smp_mailbox_t *)phys_to_virt(SMP_MAILBOX_PHYS);

    kprintf("[SMP] Starting %u Application Processor(s)...\n", g_acpi_info.cpu_count - 1);

    /* 7. Boot each Application Processor sequentially */
    for (uint32_t entry = 0; entry < g_acpi_info.cpu_count; entry++) {
        uint32_t apic_id = g_acpi_info.cpu_apic_ids[entry];
        if (!g_acpi_info.cpu_enabled[entry] || apic_id == g_cpus[0].lapic_id) {
            continue;
        }

        uint32_t i = smp_get_online_cpus();
        if (i >= SMP_MAX_CPUS) break;

        /* 16 KiB idle stack and two private 8 KiB interrupt stacks. */
        uint64_t stack_phys = pmm_alloc_pages(3);
        if (stack_phys == 0) {
            kprintf("[SMP] ERROR: Out of physical memory for CPU #%u stack!\n", i);
            break;
        }
        uint64_t stack_top = (uint64_t)phys_to_virt(stack_phys) + SMP_STACK_SIZE;

        g_cpus[i].cpu_id = i;
        g_cpus[i].lapic_id = apic_id;
        g_cpus[i].is_bsp = false;
        g_cpus[i].is_online = false;
        g_cpus[i].kernel_stack_top = stack_top;

        spin_lock(&g_smp_boot_lock);

        /* Populate mailbox */
        mb->pml4_phys = read_cr3();
        mb->ap_stack = stack_top;
        mb->ap_entry = (uint64_t)smp_ap_entry;
        mb->cpu_id = i;
        mb->status = 0;

        /* Step A: Send INIT IPI */
        lapic_send_init(apic_id);
        pit_delay_ms(10);

        /* Step B: Send Startup IPI (SIPI) with vector 0x08 -> 0x8000 */
        lapic_send_sipi(apic_id, 0x08);
        pit_delay_ms(1);

        /* Step C: If not responded, send second SIPI */
        if (mb->status == 0) {
            lapic_send_sipi(apic_id, 0x08);
            pit_delay_ms(1);
        }

        /* Wait up to 100 ms for AP to signal online */
        uint32_t timeout_ms = 100;
        while (mb->status == 0 && timeout_ms--) {
            pit_delay_ms(1);
        }

        if (mb->status == 1) {
            kprintf("[SMP] CPU #%u (APIC ID %u) booted successfully.\n", i, apic_id);
        } else {
            kprintf("[SMP] WARNING: CPU #%u (APIC ID %u) startup timed out!\n", i, apic_id);
            spin_unlock(&g_smp_boot_lock);
            break; /* A late AP must not read another CPU's mailbox. */
        }

        spin_unlock(&g_smp_boot_lock);
    }

    kprintf("[SMP] Multiprocessor startup complete: %u CPU(s) online and active.\n",
            g_smp_online_cpus);
}
