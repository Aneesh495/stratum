#ifndef STRATUM_KERNEL_MUTEX_H
#define STRATUM_KERNEL_MUTEX_H

#include <kernel/sched.h>

typedef struct mutex {
    spinlock_t guard;
    wait_queue_t waiters;
    bool held;
} mutex_t;

#define MUTEX_INIT ((mutex_t){ .guard = SPINLOCK_INIT, .waiters = { .lock = SPINLOCK_INIT } })

void mutex_init(mutex_t *m);
void mutex_lock(mutex_t *m);
bool mutex_trylock(mutex_t *m);
void mutex_unlock(mutex_t *m);

#endif /* STRATUM_KERNEL_MUTEX_H */
