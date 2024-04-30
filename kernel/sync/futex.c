#include <kernel/event.h>
#include <kernel/futex.h>
#include <kernel/process.h>
#include <shared/errno.h>
#include <shared/syscall_nums.h>

#define FUTEX_MAX_TIMEOUT_MS 60000

void futex_unmap(process_t *process, uint64_t start, uint64_t end) {
    uint64_t flags = event_lock();
    event_wake_locked(process, start, end, (unsigned)-1, -STRATUM_EFAULT);
    event_unlock(flags);
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
    if (!result) {
        uint64_t irq = event_lock();
        result = event_wait_locked(process, address, event_deadline(timeout_ms), irq);
    }
    local_irq_restore(flags);
    return result;
}

int futex_call(uint64_t address, uint64_t operation, uint64_t value, uint64_t timeout_ms) {
    if (operation != FUTEX_WAIT && operation != FUTEX_WAKE) return -STRATUM_ENOSYS;
    if (!address || address >= USER_SPACE_TOP || address % sizeof(uint32_t) || value > UINT32_MAX) {
        return -STRATUM_EINVAL;
    }
    process_t *process = process_get_current();
    if (!process) return -STRATUM_ESRCH;
    if (operation == FUTEX_WAIT) return futex_wait(process, address, (uint32_t)value, timeout_ms);
    uint64_t flags = event_lock();
    unsigned woke = event_wake_locked(process, address, address + 1, (uint32_t)value, 0);
    event_unlock(flags);
    return (int)woke;
}
