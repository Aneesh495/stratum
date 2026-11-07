#include <kernel/trace.h>
#include <kernel/smp.h>
#include <kernel/kernel.h>
#include <kernel/x86_64.h>
#include <shared/errno.h>

typedef struct {
    uint64_t sequence;
    strat_trace_record_t record;
} trace_slot_t;

static struct {
    uint64_t head;
    trace_slot_t slots[TRACE_RING_BUFFER_ENTRIES];
} traces[TRACE_MAX_CPUS];
static bool ready;

void trace_init(void) {
    ready = true;
    kprintf("[TRACE] Per-CPU trace rings initialized (capacity: %d entries/CPU).\n",
            TRACE_RING_BUFFER_ENTRIES);
}

void trace_emit(uint32_t event, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    if (!ready) return;
    uint64_t flags = local_irq_save();
    uint32_t cpu = smp_get_cpu_id();
    if (cpu < TRACE_MAX_CPUS) {
        uint64_t head = __atomic_load_n(&traces[cpu].head, __ATOMIC_RELAXED);
        trace_slot_t *slot = &traces[cpu].slots[head % TRACE_RING_BUFFER_ENTRIES];
        /* Invalidate the old record before updating a slot visible to other CPUs. */
        __atomic_store_n(&slot->sequence, 0, __ATOMIC_SEQ_CST);
        __atomic_store_n(&slot->record.tsc, rdtsc(), __ATOMIC_RELAXED);
        __atomic_store_n(&slot->record.cpu, cpu, __ATOMIC_RELAXED);
        __atomic_store_n(&slot->record.event, event, __ATOMIC_RELAXED);
        __atomic_store_n(&slot->record.args[0], a1, __ATOMIC_RELAXED);
        __atomic_store_n(&slot->record.args[1], a2, __ATOMIC_RELAXED);
        __atomic_store_n(&slot->record.args[2], a3, __ATOMIC_RELAXED);
        __atomic_store_n(&slot->record.args[3], a4, __ATOMIC_RELAXED);
        __atomic_store_n(&slot->sequence, head + 1, __ATOMIC_RELEASE);
        __atomic_store_n(&traces[cpu].head, head + 1, __ATOMIC_RELEASE);
    }
    local_irq_restore(flags);
}

int trace_info(uint32_t cpu, strat_trace_info_t *info) {
    if (cpu >= TRACE_MAX_CPUS || cpu >= smp_get_online_cpus()) return -STRATUM_EINVAL;
    uint64_t next = __atomic_load_n(&traces[cpu].head, __ATOMIC_ACQUIRE);
    uint64_t first = next > TRACE_RING_BUFFER_ENTRIES ? next - TRACE_RING_BUFFER_ENTRIES : 0;
    *info = (strat_trace_info_t){ .first = first, .next = next, .lost = first };
    return 0;
}

int trace_read(uint32_t cpu, uint64_t sequence, strat_trace_record_t *out) {
    strat_trace_info_t info;
    int result = trace_info(cpu, &info);
    if (result) return result;
    if (sequence < info.first) return -STRATUM_ERANGE;
    if (sequence >= info.next) return -STRATUM_EAGAIN;
    trace_slot_t *slot = &traces[cpu].slots[sequence % TRACE_RING_BUFFER_ENTRIES];
    if (__atomic_load_n(&slot->sequence, __ATOMIC_ACQUIRE) != sequence + 1) return -STRATUM_EAGAIN;
    strat_trace_record_t record = {
        .tsc = __atomic_load_n(&slot->record.tsc, __ATOMIC_RELAXED),
        .cpu = __atomic_load_n(&slot->record.cpu, __ATOMIC_RELAXED),
        .event = __atomic_load_n(&slot->record.event, __ATOMIC_RELAXED),
    };
    for (unsigned i = 0; i < 4; i++) record.args[i] = __atomic_load_n(&slot->record.args[i], __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    if (__atomic_load_n(&slot->sequence, __ATOMIC_ACQUIRE) != sequence + 1) return -STRATUM_EAGAIN;
    *out = record;
    return 0;
}

uint64_t trace_get_losses(uint32_t cpu) {
    strat_trace_info_t info;
    return trace_info(cpu, &info) ? 0 : info.lost;
}

void trace_dump(void) {
    kprintf("[TRACE] Dump of active CPU trace ring buffers:\n");
    for (uint32_t cpu = 0; cpu < TRACE_MAX_CPUS; cpu++) {
        strat_trace_info_t info;
        if (!trace_info(cpu, &info) && info.next) {
            kprintf("  CPU %u: Head=%lu Tail=%lu Lost=%lu\n", cpu, info.next, info.first, info.lost);
        }
    }
}
