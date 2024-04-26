#include <kernel/futex.h>
#include <kernel/process.h>
#include <kernel/smp.h>
#include <shared/errno.h>
#include <shared/syscall_nums.h>

#define FUTEX_MAX_TIMEOUT_MS 60000

typedef struct futex_waiter {
    struct futex_waiter *next;
    process_t *process;
    thread_t *thread;
    uint64_t address;
    uint64_t deadline;
    int result;
} futex_waiter_t;

static futex_waiter_t *waiters;
static spinlock_t futex_lock = SPINLOCK_INIT;

static void complete(futex_waiter_t **link, int result) {
    futex_waiter_t *waiter = *link;
    *link = waiter->next;
    waiter->result = result;
    thread_wake(waiter->thread);
}

static void expire_locked(uint64_t now) {
    while (waiters && waiters->deadline <= now) complete(&waiters, -STRATUM_ETIMEDOUT);
}

void futex_expire(uint64_t now) {
    uint64_t flags;
    spin_lock_irqsave(&futex_lock, &flags);
    expire_locked(now);
    spin_unlock_irqrestore(&futex_lock, flags);
}

void futex_unmap(process_t *process, uint64_t start, uint64_t end) {
    uint64_t flags;
    spin_lock_irqsave(&futex_lock, &flags);
    futex_waiter_t **link = &waiters;
    while (*link) {
        futex_waiter_t *waiter = *link;
        if (waiter->process == process && waiter->address >= start && waiter->address < end) {
            complete(link, -STRATUM_EFAULT);
        } else {
            link = &waiter->next;
        }
    }
    spin_unlock_irqrestore(&futex_lock, flags);
}

static int futex_wait(process_t *process, uint64_t address, uint32_t expected, uint64_t timeout_ms) {
    if (timeout_ms > FUTEX_MAX_TIMEOUT_MS) return -STRATUM_EINVAL;
    uint64_t flags = local_irq_save();
    uint32_t actual;
    /* Siblings share one CPU. IRQ exclusion closes the check/enroll window
       without faulting under the waiter lock. */
    int result = copy_from_user(&actual, (const void *)address, sizeof(actual));
    if (!result && actual != expected) result = -STRATUM_EAGAIN;
    if (!result && !timeout_ms) result = -STRATUM_ETIMEDOUT;
    if (result) {
        local_irq_restore(flags);
        return result;
    }

    futex_waiter_t waiter = {
        .process = process,
        .thread = g_runqueues[smp_get_cpu_id()].current_thread,
        .address = address,
        /* Round up and allow for entry just before a timer tick. */
        .deadline = timer_get_ticks() + (timeout_ms + 9) / 10 + 1,
    };
    spin_lock(&futex_lock);
    futex_waiter_t **link = &waiters;
    while (*link && (*link)->deadline <= waiter.deadline) link = &(*link)->next;
    waiter.thread->state = THREAD_STATE_BLOCKED;
    waiter.next = *link;
    *link = &waiter;
    spin_unlock(&futex_lock);
    sched_reschedule();
    local_irq_restore(flags);
    return waiter.result;
}

static int futex_wake(process_t *process, uint64_t address, uint32_t count) {
    uint64_t flags;
    spin_lock_irqsave(&futex_lock, &flags);
    expire_locked(timer_get_ticks());
    unsigned woke = 0;
    futex_waiter_t **link = &waiters;
    while (*link && woke < count) {
        futex_waiter_t *waiter = *link;
        if (waiter->process == process && waiter->address == address) {
            complete(link, 0);
            woke++;
        } else {
            link = &waiter->next;
        }
    }
    spin_unlock_irqrestore(&futex_lock, flags);
    return (int)woke;
}

int futex_call(uint64_t address, uint64_t operation, uint64_t value, uint64_t timeout_ms) {
    if (operation != FUTEX_WAIT && operation != FUTEX_WAKE) return -STRATUM_ENOSYS;
    if (!address || address >= USER_SPACE_TOP || address % sizeof(uint32_t) || value > UINT32_MAX) {
        return -STRATUM_EINVAL;
    }
    process_t *process = process_get_current();
    if (!process) return -STRATUM_ESRCH;
    if (operation == FUTEX_WAIT) return futex_wait(process, address, (uint32_t)value, timeout_ms);
    return futex_wake(process, address, (uint32_t)value);
}
