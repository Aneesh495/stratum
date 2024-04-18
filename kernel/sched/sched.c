#include <kernel/sched.h>
#include <kernel/process.h>
#include <kernel/smp.h>
#include <kernel/pmm.h>
#include <kernel/slab.h>
#include <kernel/gdt.h>
#include <kernel/apic.h>
#include <kernel/kernel.h>
#include <kernel/string.h>
#include <kernel/x86_64.h>

extern void sched_context_switch(uint64_t *prev_rsp_ptr, uint64_t next_rsp, uint64_t next_cr3);
extern void thread_trampoline(void);
extern void timer_tick_handler(void);
extern void timer_add_sleeper(thread_t *thread, uint32_t ms);

static thread_t   g_thread_pool[SCHED_MAX_THREADS];
static spinlock_t g_thread_pool_lock = SPINLOCK_INIT;
static uint32_t   g_next_tid = SMP_MAX_CPUS + 1;

runqueue_t g_runqueues[SMP_MAX_CPUS];

static void rq_enqueue(runqueue_t *rq, thread_t *t) {
    if (!rq || !t) return;
    t->state = THREAD_STATE_READY;
    t->next = NULL;

    thread_prio_t p = t->priority;
    if (rq->tails[p]) {
        rq->tails[p]->next = t;
        rq->tails[p] = t;
    } else {
        rq->queues[p] = t;
        rq->tails[p] = t;
    }
    rq->nr_running++;
}

static thread_t *rq_dequeue(runqueue_t *rq) {
    if (!rq) return NULL;

    /* Search highest priority to lowest priority (realtime down to low) */
    for (int p = THREAD_PRIO_REALTIME; p >= THREAD_PRIO_LOW; p--) {
        if (rq->queues[p]) {
            thread_t *t = rq->queues[p];
            rq->queues[p] = t->next;
            if (!rq->queues[p]) {
                rq->tails[p] = NULL;
            }
            t->next = NULL;
            if (rq->nr_running > 0) rq->nr_running--;
            return t;
        }
    }

    return rq->idle_thread;
}

static thread_t *rq_steal_work(uint32_t my_cpu) {
    uint32_t online = smp_get_online_cpus();
    for (uint32_t i = 0; i < online; i++) {
        if (i == my_cpu) continue;

        runqueue_t *other_rq = &g_runqueues[i];
        /* Our runqueue is already locked, so never wait on another one. */
        if (spin_trylock(&other_rq->lock)) {
            thread_t *stolen = NULL;
            for (int p = THREAD_PRIO_HIGH; p >= THREAD_PRIO_LOW; p--) {
                thread_t *prev = NULL;
                thread_t *curr = other_rq->queues[p];
                while (curr) {
                    if (!curr->pinned && !curr->on_cpu && curr->state == THREAD_STATE_READY) {
                        /* Dequeue curr from other_rq */
                        if (prev) {
                            prev->next = curr->next;
                        } else {
                            other_rq->queues[p] = curr->next;
                        }
                        if (other_rq->tails[p] == curr) {
                            other_rq->tails[p] = prev;
                        }
                        curr->next = NULL;
                        other_rq->nr_running--;
                        curr->assigned_cpu = my_cpu;
                        stolen = curr;
                        break;
                    }
                    prev = curr;
                    curr = curr->next;
                }
                if (stolen) break;
            }
            spin_unlock(&other_rq->lock);
            if (stolen) return stolen;
        }
    }
    return NULL;
}

void sched_finish_switch(void) {
    uint32_t cpu_id = smp_get_cpu_id();
    runqueue_t *rq = &g_runqueues[cpu_id];
    thread_t *prev = rq->prev_thread;
    if (prev) {
        spin_lock(&rq->lock);
        prev->on_cpu = false;
        rq->prev_thread = NULL;
        spin_unlock(&rq->lock);
        if (prev->state == THREAD_STATE_ZOMBIE) {
            process_t *process = prev->process;
            if (prev->kernel_stack_base) {
                pmm_free_pages(virt_to_phys((void *)prev->kernel_stack_base), 2);
                prev->kernel_stack_base = 0;
            }
            spin_lock(&g_thread_pool_lock);
            prev->state = THREAD_STATE_UNUSED;
            spin_unlock(&g_thread_pool_lock);
            if (process) process_finish_exit(process);
        }
    }
}

void sched_init_cpu(uint32_t cpu_id) {
    /* Floating-point state is unavailable until it is saved per thread. */
    write_cr0(read_cr0() | CR0_WP | CR0_TS);
    runqueue_t *rq = &g_runqueues[cpu_id];
    memset(rq, 0, sizeof(runqueue_t));
    spin_lock_init(&rq->lock);

    /* Create idle thread for this core */
    char idle_name[32];
    snprintf(idle_name, sizeof(idle_name), "idle/%u", cpu_id);

    thread_t *idle = &g_thread_pool[cpu_id];
    memset(idle, 0, sizeof(thread_t));
    idle->tid = cpu_id + 1;
    strncpy(idle->name, idle_name, sizeof(idle->name) - 1);
    idle->state = THREAD_STATE_RUNNING;
    idle->priority = THREAD_PRIO_IDLE;
    idle->assigned_cpu = cpu_id;
    idle->on_cpu = true;

    /* Use CPU's default kernel stack */
    cpu_t *cpu = smp_get_current_cpu();
    idle->kernel_stack_top = cpu->kernel_stack_top;

    rq->idle_thread = idle;
    rq->current_thread = idle;
}

void sched_init(void) {
    /* Initialize BSP runqueue */
    sched_init_cpu(0);

    /* Register APIC timer interrupt (vector 0x20) */
    register_interrupt_handler(VEC_APIC_TIMER, sched_tick);

    kprintf("[SCHED] Preemptive SMP Scheduler initialized (O(1) priority queues, work stealing enabled)\n");
}

static thread_t *thread_create_internal(const char *name, void (*entry)(void *), void *arg,
                                        thread_prio_t prio, pml4_t *as, process_t *process,
                                        int fixed_cpu) {
    if (!entry) return NULL;

    uint64_t flags;
    spin_lock_irqsave(&g_thread_pool_lock, &flags);

    thread_t *t = NULL;
    for (uint32_t i = SMP_MAX_CPUS; i < SCHED_MAX_THREADS; i++) {
        if (g_thread_pool[i].state == THREAD_STATE_UNUSED) {
            t = &g_thread_pool[i];
            break;
        }
    }

    if (!t) {
        spin_unlock_irqrestore(&g_thread_pool_lock, flags);
        kprintf("[SCHED] ERROR: Thread table full!\n");
        return NULL;
    }

    memset(t, 0, sizeof(thread_t));
    t->tid = g_next_tid++;
    strncpy(t->name, name ? name : "kthread", sizeof(t->name) - 1);
    t->priority = prio;
    t->on_cpu = false;
    t->pinned = fixed_cpu >= 0;
    t->address_space = as;
    t->process = process;

    /* Allocate 16 KiB stack */
    uint64_t stack_phys = pmm_alloc_pages(2);
    if (!stack_phys) {
        spin_unlock_irqrestore(&g_thread_pool_lock, flags);
        return NULL;
    }

    t->kernel_stack_base = (uint64_t)phys_to_virt(stack_phys);
    t->kernel_stack_top = t->kernel_stack_base + THREAD_STACK_SIZE;

    /* Prepare stack frame for context switch into thread_trampoline */
    uint64_t *sp = (uint64_t *)t->kernel_stack_top;
    *(--sp) = (uint64_t)arg;                /* Arg to pop in trampoline */
    *(--sp) = (uint64_t)entry;              /* Entry to call */
    *(--sp) = (uint64_t)thread_trampoline;  /* RIP for ret */
    *(--sp) = 0x002;                        /* Enable IRQs after finishing the switch */
    *(--sp) = 0;                            /* RBP */
    *(--sp) = 0;                            /* RBX */
    *(--sp) = 0;                            /* R12 */
    *(--sp) = 0;                            /* R13 */
    *(--sp) = 0;                            /* R14 */
    *(--sp) = 0;                            /* R15 */
    t->saved_rsp = (uint64_t)sp;

    /* Pick target CPU with lowest load */
    uint32_t target_cpu = t->pinned ? (uint32_t)fixed_cpu : 0;
    uint32_t min_running = 0xFFFFFFFF;
    uint32_t online = smp_get_online_cpus();
    if (online == 0) online = 1;

    for (uint32_t c = 0; !t->pinned && c < online; c++) {
        spin_lock(&g_runqueues[c].lock);
        uint32_t running = g_runqueues[c].nr_running;
        spin_unlock(&g_runqueues[c].lock);
        if (running < min_running) {
            min_running = running;
            target_cpu = c;
        }
    }

    t->assigned_cpu = target_cpu;
    if (process) process->main_thread = t;

    runqueue_t *rq = &g_runqueues[target_cpu];
    uint64_t rq_flags;
    spin_lock_irqsave(&rq->lock, &rq_flags);
    rq_enqueue(rq, t);
    spin_unlock_irqrestore(&rq->lock, rq_flags);

    spin_unlock_irqrestore(&g_thread_pool_lock, flags);

    /* Signal target CPU if not current CPU */
    if (target_cpu != smp_get_cpu_id()) {
        smp_send_ipi(g_cpus[target_cpu].lapic_id, VEC_IPI_RESCHED);
    }

    return t;
}

thread_t *thread_create_on_cpu(const char *name, void (*entry)(void *), void *arg,
                               thread_prio_t prio, uint32_t cpu) {
    if (cpu >= smp_get_online_cpus()) return NULL;
    return thread_create_internal(name, entry, arg, prio, NULL, NULL, (int)cpu);
}

thread_t *thread_create_user(const char *name, void (*entry)(void *), void *arg,
                             thread_prio_t prio, pml4_t *as, process_t *process) {
    return thread_create_internal(name, entry, arg, prio, as, process, -1);
}

thread_t *thread_create(const char *name, void (*entry)(void *), void *arg, thread_prio_t prio) {
    return thread_create_user(name, entry, arg, prio, NULL, NULL);
}

void sched_reschedule(void) {
    uint64_t flags = local_irq_save();

    uint32_t cpu_id = smp_get_cpu_id();
    runqueue_t *rq = &g_runqueues[cpu_id];

    spin_lock(&rq->lock);

    thread_t *curr = rq->current_thread;
    thread_t *next = rq_dequeue(rq);

    if (next == rq->idle_thread && rq->nr_running == 0) {
        /* Try work stealing */
        thread_t *stolen = rq_steal_work(cpu_id);
        if (stolen) {
            next = stolen;
        }
    }

    if (next == rq->idle_thread && curr->state == THREAD_STATE_RUNNING && curr != rq->idle_thread) {
        /* Current thread is still runnable and queue is empty: keep running */
        curr->timeslice_remaining = SCHED_DEFAULT_TIMESLICE;
        spin_unlock(&rq->lock);
        local_irq_restore(flags);
        return;
    }

    if (curr == next) {
        curr->state = THREAD_STATE_RUNNING;
        curr->timeslice_remaining = SCHED_DEFAULT_TIMESLICE;
        spin_unlock(&rq->lock);
        local_irq_restore(flags);
        return;
    }

    if (curr->state == THREAD_STATE_RUNNING) {
        if (curr != rq->idle_thread) {
            rq_enqueue(rq, curr);
        } else {
            curr->state = THREAD_STATE_READY;
        }
    }

    next->state = THREAD_STATE_RUNNING;
    next->timeslice_remaining = SCHED_DEFAULT_TIMESLICE;
    next->on_cpu = true;
    rq->current_thread = next;
    rq->prev_thread = curr;

    /* Update TSS kernel stack */
    gdt_set_kernel_stack(next->kernel_stack_top);

    spin_unlock(&rq->lock);

    /* Perform context switch */
    uint64_t next_cr3 = next->address_space ? virt_to_phys(next->address_space) : virt_to_phys(g_kernel_pml4);
    sched_context_switch(&curr->saved_rsp, next->saved_rsp, next_cr3);

    sched_finish_switch();

    local_irq_restore(flags);
}

void thread_yield(void) {
    sched_reschedule();
}

void thread_sleep_ms(uint32_t ms) {
    uint64_t flags = local_irq_save();
    uint32_t cpu_id = smp_get_cpu_id();
    thread_t *curr = g_runqueues[cpu_id].current_thread;
    if (curr) {
        timer_add_sleeper(curr, ms);
        sched_reschedule();
    }
    local_irq_restore(flags);
}

void thread_wake(thread_t *thread) {
    if (!thread) return;

    runqueue_t *rq = &g_runqueues[thread->assigned_cpu];
    uint64_t flags;
    spin_lock_irqsave(&rq->lock, &flags);
    rq_enqueue(rq, thread);

    uint32_t target_cpu = thread->assigned_cpu;
    spin_unlock_irqrestore(&rq->lock, flags);

    /* The caller may still hold the lock protecting the wait condition. */
    if (target_cpu != smp_get_cpu_id()) {
        smp_send_ipi(g_cpus[target_cpu].lapic_id, VEC_IPI_RESCHED);
    }
}

void wait_queue_init(wait_queue_t *wq) {
    if (!wq) return;
    spin_lock_init(&wq->lock);
    wq->head = NULL;
}

void wait_queue_wait_locked(wait_queue_t *wq, spinlock_t *condition, uint64_t flags) {
    thread_t *curr = g_runqueues[smp_get_cpu_id()].current_thread;
    spin_lock(&wq->lock);
    curr->state = THREAD_STATE_BLOCKED;
    curr->wait_next = wq->head;
    wq->head = curr;
    spin_unlock(&wq->lock);
    spin_unlock(condition);
    sched_reschedule();
    local_irq_restore(flags);
}

void wait_queue_wake_one(wait_queue_t *wq) {
    if (!wq) return;
    uint64_t flags;
    spin_lock_irqsave(&wq->lock, &flags);
    thread_t *t = wq->head;
    if (t) {
        wq->head = t->wait_next;
        t->wait_next = NULL;
        spin_unlock_irqrestore(&wq->lock, flags);
        thread_wake(t);
        return;
    }
    spin_unlock_irqrestore(&wq->lock, flags);
}

void wait_queue_wake_all(wait_queue_t *wq) {
    if (!wq) return;
    uint64_t flags;
    spin_lock_irqsave(&wq->lock, &flags);
    thread_t *list = wq->head;
    wq->head = NULL;
    spin_unlock_irqrestore(&wq->lock, flags);

    while (list) {
        thread_t *next = list->wait_next;
        list->wait_next = NULL;
        thread_wake(list);
        list = next;
    }
}

void thread_exit(void) {
    cli();
    uint32_t cpu_id = smp_get_cpu_id();
    thread_t *curr = g_runqueues[cpu_id].current_thread;
    if (curr) {
        curr->state = THREAD_STATE_ZOMBIE;
    }
    sched_reschedule();
    while (1) {
        hlt();
    }
}

void sched_tick(interrupt_frame_t *frame) {
    (void)frame;
    uint32_t cpu_id = smp_get_cpu_id();

    /* Monotonic timekeeping driven by BSP */
    if (cpu_id == 0) {
        timer_tick_handler();
    }

    runqueue_t *rq = &g_runqueues[cpu_id];
    thread_t *curr = rq->current_thread;

    bool need_resched = false;
    spin_lock(&rq->lock);
    if (curr == rq->idle_thread && rq->nr_running > 0) {
        need_resched = true;
    } else if (curr && curr != rq->idle_thread) {
        curr->runtime_ticks++;
        if (curr->timeslice_remaining > 0) {
            curr->timeslice_remaining--;
        }
        if (curr->timeslice_remaining == 0) {
            need_resched = true;
        } else {
            /* Preempt if higher priority thread is waiting */
            for (int p = THREAD_PRIO_REALTIME; p > (int)curr->priority; p--) {
                if (rq->queues[p]) {
                    need_resched = true;
                    break;
                }
            }
        }
    }

    spin_unlock(&rq->lock);
    if (need_resched) {
        sched_reschedule();
    }
}
