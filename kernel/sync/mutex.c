#include <kernel/mutex.h>

void mutex_init(mutex_t *m) {
    spin_lock_init(&m->guard);
    wait_queue_init(&m->waiters);
    m->held = false;
}

void mutex_lock(mutex_t *m) {
    for (;;) {
        uint64_t flags;
        spin_lock_irqsave(&m->guard, &flags);
        if (!m->held) {
            m->held = true;
            spin_unlock_irqrestore(&m->guard, flags);
            return;
        }
        /* A cancelled task can still hold other mutexes while it unwinds. */
        wait_queue_wait_uninterruptible(&m->waiters, &m->guard, flags);
    }
}

bool mutex_trylock(mutex_t *m) {
    uint64_t flags;
    spin_lock_irqsave(&m->guard, &flags);
    bool acquired = !m->held;
    if (acquired) m->held = true;
    spin_unlock_irqrestore(&m->guard, flags);
    return acquired;
}

void mutex_unlock(mutex_t *m) {
    uint64_t flags;
    spin_lock_irqsave(&m->guard, &flags);
    m->held = false;
    wait_queue_wake_one(&m->waiters);
    spin_unlock_irqrestore(&m->guard, flags);
}
