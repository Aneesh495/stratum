#include <kernel/sched.h>
#include <kernel/event.h>
#include <kernel/spinlock.h>

static volatile uint64_t g_timer_ticks = 0;
static thread_t         *g_sleep_queue = NULL;
static spinlock_t        g_sleep_lock = SPINLOCK_INIT;

uint64_t timer_get_ticks(void) {
    return __atomic_load_n(&g_timer_ticks, __ATOMIC_RELAXED);
}

uint64_t timer_get_uptime_ms(void) {
    return timer_get_ticks() * 10;
}

void timer_add_sleeper(thread_t *thread, uint32_t ms) {
    if (!thread) return;

    /* Allow for entry immediately before a timer tick. */
    uint64_t delta_ticks = ((uint64_t)ms + 9) / 10 + 1;

    uint64_t flags;
    spin_lock_irqsave(&g_sleep_lock, &flags);

    thread->sleep_until_ticks = timer_get_ticks() + delta_ticks;
    thread->state = THREAD_STATE_SLEEPING;
    thread_t **slot = &g_sleep_queue;
    while (*slot && (*slot)->sleep_until_ticks <= thread->sleep_until_ticks) slot = &(*slot)->next;
    thread->next = *slot;
    *slot = thread;

    spin_unlock_irqrestore(&g_sleep_lock, flags);
}

void timer_cancel_sleeper(thread_t *thread) {
    uint64_t flags;
    spin_lock_irqsave(&g_sleep_lock, &flags);
    for (thread_t **link = &g_sleep_queue; *link; link = &(*link)->next) {
        if (*link == thread) {
            *link = thread->next;
            thread->next = NULL;
            thread_wake(thread);
            break;
        }
    }
    spin_unlock_irqrestore(&g_sleep_lock, flags);
}

void timer_check_sleepers(void) {
    uint64_t flags;
    spin_lock_irqsave(&g_sleep_lock, &flags);

    uint64_t now = timer_get_ticks();
    while (g_sleep_queue && g_sleep_queue->sleep_until_ticks <= now) {
        thread_t *thread = g_sleep_queue;
        g_sleep_queue = thread->next;
        thread->next = NULL;
        thread_wake(thread);
    }

    spin_unlock_irqrestore(&g_sleep_lock, flags);
}

void timer_tick_handler(void) {
    uint64_t now = __atomic_add_fetch(&g_timer_ticks, 1, __ATOMIC_SEQ_CST);
    event_expire(now);
    timer_check_sleepers();
}
