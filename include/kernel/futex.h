#ifndef STRATUM_KERNEL_FUTEX_H
#define STRATUM_KERNEL_FUTEX_H

#include <kernel/types.h>

struct process;

int futex_call(uint64_t address, uint64_t operation, uint64_t value, uint64_t timeout_ms);
void futex_expire(uint64_t now);
void futex_unmap(struct process *process, uint64_t start, uint64_t end);

#endif
