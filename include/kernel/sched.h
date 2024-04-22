#ifndef STRATUM_KERNEL_SCHED_H
#define STRATUM_KERNEL_SCHED_H

#include <kernel/types.h>
#include <kernel/spinlock.h>
#include <kernel/vmm.h>
#include <kernel/idt.h>

#define THREAD_STACK_SIZE      16384 /* 16 KiB per thread kernel stack */
#define SCHED_DEFAULT_TIMESLICE 5     /* 5 timer ticks (~50ms) */
#define SCHED_MAX_THREADS      1024

typedef enum {
    THREAD_STATE_UNUSED = 0,
    THREAD_STATE_READY,
    THREAD_STATE_RUNNING,
    THREAD_STATE_BLOCKED,
    THREAD_STATE_SLEEPING,
    THREAD_STATE_ZOMBIE,
} thread_state_t;

typedef enum {
    THREAD_PRIO_IDLE = 0,
    THREAD_PRIO_LOW,
    THREAD_PRIO_NORMAL,
    THREAD_PRIO_HIGH,
    THREAD_PRIO_REALTIME,
    THREAD_PRIO_COUNT
} thread_prio_t;

typedef struct thread {
    uint32_t         tid;
    char             name[32];
    thread_state_t   state;
    thread_prio_t    priority;
    uint32_t         assigned_cpu;
    volatile bool    on_cpu;
    bool             pinned;

    uint64_t         timeslice_remaining;
    uint64_t         runtime_ticks;
    uint64_t         sleep_until_ticks;

    uint64_t         kernel_stack_base;
    uint64_t         kernel_stack_top;
    uint64_t         saved_rsp;

    pml4_t          *address_space; /* CR3 address space */
    struct process  *process;       /* Owning process */
    uint32_t         worker_slot;   /* Zero for the original process thread */
    int              exit_status;
    uint64_t         fs_base;
    _Alignas(16) uint8_t fp_state[512];

    struct thread   *next;          /* Queue link */
    struct thread   *global_next;   /* Global thread table link */
    struct thread   *wait_next;     /* Wait queue link */
} thread_t;

typedef struct wait_queue {
    spinlock_t     lock;
    struct thread *head;
} wait_queue_t;

typedef struct {
    spinlock_t lock;
    thread_t  *queues[THREAD_PRIO_COUNT];
    thread_t  *tails[THREAD_PRIO_COUNT];
    uint32_t   nr_running;
    uint64_t   user_switches;
    thread_t  *current_thread;
    thread_t  *idle_thread;
    thread_t  *prev_thread;
} runqueue_t;

extern runqueue_t g_runqueues[];

void      sched_init(void);
void      sched_init_cpu(uint32_t cpu_id);
void      sched_finish_switch(void);
thread_t *thread_create(const char *name, void (*entry)(void *), void *arg, thread_prio_t prio);
thread_t *thread_create_on_cpu(const char *name, void (*entry)(void *), void *arg, thread_prio_t prio, uint32_t cpu);
thread_t *thread_create_user(const char *name, void (*entry)(void *), void *arg, thread_prio_t prio, pml4_t *as, struct process *process,
                                 thread_t *inherit, int fixed_cpu);
thread_t *thread_create_worker(struct process *process, uint32_t slot, uint64_t fs_base,
                               void (*entry)(void *), void *arg);
void      thread_reset_user_state(thread_t *thread);
void      thread_exit(void);
void      thread_yield(void);
void      thread_sleep_ms(uint32_t ms);
void      thread_wake(thread_t *thread);

void      wait_queue_init(wait_queue_t *wq);
void      wait_queue_wait_locked(wait_queue_t *wq, spinlock_t *condition, uint64_t flags);
void      wait_queue_wake_one(wait_queue_t *wq);
void      wait_queue_wake_all(wait_queue_t *wq);

void      sched_tick(interrupt_frame_t *frame);
void      sched_reschedule(void);

uint64_t  timer_get_ticks(void);
uint64_t  timer_get_uptime_ms(void);

#endif /* STRATUM_KERNEL_SCHED_H */
