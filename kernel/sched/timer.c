#include <kernel/sched.h>
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

    uint64_t delta_ticks = ((uint64_t)ms + 9) / 10;
    if (delta_ticks == 0) delta_ticks = 1;

    uint64_t flags;
    spin_lock_irqsave(&g_sleep_lock, &flags);

    thread->sleep_until_ticks = timer_get_ticks() + delta_ticks;
    thread->state = THREAD_STATE_SLEEPING;
    thread->next = g_sleep_queue;
    g_sleep_queue = thread;

    spin_unlock_irqrestore(&g_sleep_lock, flags);
}

void timer_check_sleepers(void) {
    uint64_t flags;
    spin_lock_irqsave(&g_sleep_lock, &flags);

    thread_t **curr = &g_sleep_queue;
    while (*curr) {
        thread_t *t = *curr;
        if (timer_get_ticks() >= t->sleep_until_ticks) {
            *curr = t->next;
            t->next = NULL;
            thread_wake(t);
        } else {
            curr = &t->next;
        }
    }

    spin_unlock_irqrestore(&g_sleep_lock, flags);
}

void timer_tick_handler(void) {
    __atomic_add_fetch(&g_timer_ticks, 1, __ATOMIC_SEQ_CST);
    timer_check_sleepers();
}
