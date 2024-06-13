#ifndef STRATUM_KERNEL_TRACE_H
#define STRATUM_KERNEL_TRACE_H

#include <kernel/types.h>
#include <shared/syscall_nums.h>

#define TRACE_RING_BUFFER_ENTRIES 1024
#define TRACE_MAX_CPUS           32

/* Standard Trace Event IDs */
#define TRACE_EVENT_NONE         0
#define TRACE_EVENT_SCHED_SWITCH 1
#define TRACE_EVENT_SYSCALL      2
#define TRACE_EVENT_PAGE_FAULT   3
#define TRACE_EVENT_BLOCK_IO     4
#define TRACE_EVENT_NET_PACKET   5
#define TRACE_EVENT_LEDGER_TX    6
#define TRACE_EVENT_LEDGER_BLOCK 7
#define TRACE_EVENT_USER_CUSTOM  STRAT_TRACE_USER_BASE

void trace_init(void);
void trace_emit(uint32_t event_id, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4);
uint64_t trace_get_losses(uint32_t cpu_id);
int trace_info(uint32_t cpu, strat_trace_info_t *info);
int trace_read(uint32_t cpu, uint64_t sequence, strat_trace_record_t *out);
void trace_dump(void);

#endif /* STRATUM_KERNEL_TRACE_H */
