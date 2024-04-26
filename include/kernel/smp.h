#ifndef STRATUM_KERNEL_SMP_H
#define STRATUM_KERNEL_SMP_H

#include <kernel/types.h>
#include <kernel/gdt.h>
#include <kernel/apic.h>
#include <kernel/spinlock.h>
#include <shared/boot_info.h>

#define SMP_MAX_CPUS        32
#define SMP_STACK_SIZE      16384 /* 16 KiB per-CPU kernel stack */
#define SMP_TRAMPOLINE_PHYS 0x8000ULL
#define SMP_MAILBOX_PHYS    0x8F00ULL

typedef struct {
    uint64_t pml4_phys;
    uint64_t ap_stack;
    uint64_t ap_entry;
    uint32_t cpu_id;
    volatile uint32_t status;
} __attribute__((packed)) smp_mailbox_t;

typedef struct __attribute__((aligned(64))) cpu {
    uint64_t          kernel_stack;     /* Offset 0: active kernel stack top for syscall */
    uint64_t          user_rsp;         /* Offset 8: scratch for user rsp during syscall */
    uint32_t          cpu_id;
    uint32_t          lapic_id;
    volatile bool     is_bsp;
    volatile bool     is_online;
    uint64_t          kernel_stack_top;

    tss_t             tss;
    uint64_t          gdt_entries[7];
    gdt_desc_t        gdt_desc;

    uint64_t          ticks;
    void             *current_thread;
    void             *idle_thread;

    uint64_t tlb_shootdown_addr;
    uint64_t tlb_shootdown_request;
    uint64_t tlb_shootdown_ack;
} cpu_t;

extern cpu_t g_cpus[SMP_MAX_CPUS];
extern volatile uint32_t g_smp_online_cpus;

void     smp_init(const boot_handoff_t *handoff);
cpu_t   *smp_get_current_cpu(void);
uint32_t smp_get_cpu_id(void);
uint32_t smp_get_online_cpus(void);

void     smp_send_ipi(uint32_t dest_apic_id, uint8_t vector);
void     smp_broadcast_ipi(uint8_t vector, bool include_self);
int      smp_tlb_shootdown(uint64_t vaddr);
void     smp_halt_all(void);

void     smp_ap_entry(uint64_t cpu_id);

#endif /* STRATUM_KERNEL_SMP_H */
