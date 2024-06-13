#include <kernel/event.h>
#include <kernel/sched.h>
#include <kernel/smp.h>
#include <shared/errno.h>

typedef struct event_waiter {
    struct event_waiter *next;
    const void *owner;
    thread_t *thread;
    uint64_t key;
    uint64_t deadline;
    int result;
    bool interruptible;
} event_waiter_t;

static event_waiter_t *waiters;
static spinlock_t lock = SPINLOCK_INIT;
static uint64_t readiness;

uint64_t event_lock(void) {
    uint64_t flags;
    spin_lock_irqsave(&lock, &flags);
    return flags;
}

void event_unlock(uint64_t flags) {
    spin_unlock_irqrestore(&lock, flags);
}

static void complete(event_waiter_t **link, int result) {
    event_waiter_t *waiter = *link;
    *link = waiter->next;
    waiter->result = result;
    thread_wake(waiter->thread);
}

static void expire_locked(uint64_t now) {
    while (waiters && waiters->deadline <= now) complete(&waiters, -STRATUM_ETIMEDOUT);
}

void event_expire(uint64_t now) {
    uint64_t flags = event_lock();
    expire_locked(now);
    event_unlock(flags);
}

uint64_t event_deadline(uint64_t timeout_ms) {
    /* Round up and allow for entry just before a timer tick. */
    return timer_get_ticks() + (timeout_ms + 9) / 10 + 1;
}

static int wait_event(const void *owner, uint64_t key, uint64_t deadline, uint64_t flags, bool interruptible) {
    event_waiter_t waiter = {
        .owner = owner,
        .thread = g_runqueues[smp_get_cpu_id()].current_thread,
        .key = key,
        .deadline = deadline,
        .interruptible = interruptible,
    };
    event_waiter_t **link = &waiters;
    while (*link && (*link)->deadline <= deadline) link = &(*link)->next;
    if (interruptible && waiter.thread->cancelled) {
        event_unlock(flags);
        return -STRATUM_EINTR;
    }
    waiter.thread->state = THREAD_STATE_BLOCKED;
    waiter.next = *link;
    *link = &waiter;
    spin_unlock(&lock);
    sched_reschedule();
    local_irq_restore(flags);
    return waiter.result;
}

int event_wait_locked(const void *owner, uint64_t key, uint64_t deadline, uint64_t flags) {
    return wait_event(owner, key, deadline, flags, true);
}

int event_wait_uninterruptible(const void *owner, uint64_t key, uint64_t deadline, uint64_t flags) {
    return wait_event(owner, key, deadline, flags, false);
}

unsigned event_wake_locked(const void *owner, uint64_t start, uint64_t end, unsigned count, int result) {
    expire_locked(timer_get_ticks());
    unsigned woke = 0;
    event_waiter_t **link = &waiters;
    while (*link && woke < count) {
        event_waiter_t *waiter = *link;
        if (waiter->owner == owner && waiter->key >= start && waiter->key < end) {
            complete(link, result);
            woke++;
        } else {
            link = &waiter->next;
        }
    }
    return woke;
}

void event_cancel(thread_t *thread) {
    uint64_t flags = event_lock();
    for (event_waiter_t **link = &waiters; *link; link = &(*link)->next) {
        if ((*link)->thread == thread && (*link)->interruptible) {
            complete(link, -STRATUM_EINTR);
            break;
        }
    }
    event_unlock(flags);
}

uint64_t readiness_generation(void) {
    return __atomic_load_n(&readiness, __ATOMIC_ACQUIRE);
}

void readiness_notify(void) {
    uint64_t flags = event_lock();
    __atomic_add_fetch(&readiness, 1, __ATOMIC_RELEASE);
    event_wake_locked(&readiness, 0, 1, (unsigned)-1, 0);
    event_unlock(flags);
}

int readiness_wait(uint64_t generation, uint64_t deadline) {
    uint64_t flags = event_lock();
    if (readiness_generation() == generation && timer_get_ticks() < deadline) {
        return event_wait_locked(&readiness, 0, deadline, flags);
    }
    event_unlock(flags);
    return 0;
}
