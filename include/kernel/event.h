#ifndef STRATUM_KERNEL_EVENT_H
#define STRATUM_KERNEL_EVENT_H

#include <kernel/types.h>

uint64_t event_lock(void);
void event_unlock(uint64_t flags);
int event_wait_locked(const void *owner, uint64_t key, uint64_t deadline, uint64_t flags);
int event_wait_uninterruptible(const void *owner, uint64_t key, uint64_t deadline, uint64_t flags);
unsigned event_wake_locked(const void *owner, uint64_t start, uint64_t end, unsigned count, int result);
uint64_t event_deadline(uint64_t timeout_ms);
void event_expire(uint64_t now);
struct thread;
void event_cancel(struct thread *thread);

uint64_t readiness_generation(void);
void readiness_notify(void);
int readiness_wait(uint64_t generation, uint64_t deadline);

#endif
