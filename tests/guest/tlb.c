#include <kernel/kernel.h>
#include <kernel/pmm.h>
#include <kernel/sched.h>
#include <kernel/smp.h>

#define TLB_CYCLES 10000
#define TLB_PROBE_ADDRESS 0xFFFF900000100000ULL
#define RETIRED_PATTERN 0xaabbddeeffcc0022ULL

static volatile uint64_t *const probe = (void *)TLB_PROBE_ADDRESS;
static unsigned stage;
static unsigned seen[SMP_MAX_CPUS];
static unsigned done;

static void tlb_worker(void *arg) {
    unsigned cpu = smp_get_cpu_id();
    kassert(cpu == (uintptr_t)arg);
    for (unsigned i = 1; i <= TLB_CYCLES; i++) {
        while (__atomic_load_n(&stage, __ATOMIC_ACQUIRE) != 2 * i - 1) cpu_pause();
        kassert(probe[0] == i - 1); /* Cache the old translation on this CPU. */
        __atomic_store_n(&seen[cpu], 2 * i - 1, __ATOMIC_RELEASE);
        while (__atomic_load_n(&stage, __ATOMIC_ACQUIRE) != 2 * i) cpu_pause();
        kassert(probe[0] == i);
        probe[cpu + 1] = (uint64_t)i << 32 | cpu;
        __atomic_store_n(&seen[cpu], 2 * i, __ATOMIC_RELEASE);
    }
    __atomic_add_fetch(&done, 1, __ATOMIC_RELEASE);
}

static void rendezvous(unsigned online, unsigned sequence) {
    __atomic_store_n(&stage, sequence, __ATOMIC_RELEASE);
    uint64_t started = timer_get_uptime_ms();
    for (unsigned cpu = 1; cpu < online; cpu++) {
        while (__atomic_load_n(&seen[cpu], __ATOMIC_ACQUIRE) != sequence) {
            kassert(timer_get_uptime_ms() - started < 5000);
            cpu_pause();
        }
    }
}

void tlb_campaign(void) {
    unsigned online = smp_get_online_cpus();
    kassert(online > 1);
    uint64_t flags = PTE_WRITABLE | PTE_NX;
    uint64_t active = pmm_alloc_page(), spare = pmm_alloc_page();
    kassert(active && spare);
    ((uint64_t *)phys_to_virt(active))[0] = 0;
    kassert(vmm_map_page(g_kernel_pml4, TLB_PROBE_ADDRESS, active, flags) == 0);
    for (unsigned cpu = 1; cpu < online; cpu++) {
        kassert(thread_create_on_cpu("tlb-probe", tlb_worker, (void *)(uintptr_t)cpu, THREAD_PRIO_NORMAL, cpu));
    }

    for (unsigned cycle = 1; cycle <= TLB_CYCLES; cycle++) {
        rendezvous(online, 2 * cycle - 1);
        kassert(probe[0] == cycle - 1);
        uint64_t *old = phys_to_virt(active);
        uint64_t *next = phys_to_virt(spare);
        next[0] = cycle;
        for (unsigned cpu = 0; cpu < online; cpu++) next[cpu + 1] = 0;
        kassert(vmm_map_page(g_kernel_pml4, TLB_PROBE_ADDRESS, spare, flags) == 0);
        kassert(smp_tlb_shootdown(TLB_PROBE_ADDRESS) == 0);

        /* Reuse the retired frame immediately, only after every CPU acknowledges. */
        for (unsigned j = 0; j <= online; j++) old[j] = RETIRED_PATTERN;
        kassert(probe[0] == cycle);
        probe[1] = (uint64_t)cycle << 32;
        rendezvous(online, 2 * cycle);
        for (unsigned cpu = 0; cpu < online; cpu++) {
            kassert(next[cpu + 1] == ((uint64_t)cycle << 32 | cpu));
            kassert(old[cpu + 1] == RETIRED_PATTERN);
        }
        kassert(old[0] == RETIRED_PATTERN);
        uint64_t retired = active;
        active = spare;
        spare = retired;
    }

    uint64_t started = timer_get_uptime_ms();
    while (__atomic_load_n(&done, __ATOMIC_ACQUIRE) != online - 1) {
        kassert(timer_get_uptime_ms() - started < 5000);
        cpu_pause();
    }
    kassert(vmm_unmap_page(g_kernel_pml4, TLB_PROBE_ADDRESS) == 0);
    kassert(smp_tlb_shootdown(TLB_PROBE_ADDRESS) == 0);
    pmm_free_page(active);
    pmm_free_page(spare);
    kprintf("[TLB] cycles=%u CPUs=%u retired frames verified, final unmap acknowledged.\n", TLB_CYCLES, online);
}
