#include <kernel/process.h>
#include <kernel/pmm.h>
#include <kernel/futex.h>
#include <kernel/slab.h>
#include <kernel/kernel.h>
#include <kernel/string.h>
#include <kernel/smp.h>
#include <kernel/vmm.h>
#include <shared/errno.h>
#include <kernel/x86_64.h>

#define WORKER_STACK_PAGES 4

static process_t g_process_pool[PROCESS_MAX_COUNT];
static spinlock_t g_process_lock = SPINLOCK_INIT;
static uint32_t   g_next_pid = 1;

void process_init(void) {
    memset(g_process_pool, 0, sizeof(g_process_pool));
    for (int i = 0; i < PROCESS_MAX_COUNT; i++) {
        spin_lock_init(&g_process_pool[i].lock);
        wait_queue_init(&g_process_pool[i].wait_child);
    }
}

static thread_t *current_thread(void) {
    return g_runqueues[smp_get_cpu_id()].current_thread;
}

process_t *process_get_current(void) {
    thread_t *curr = current_thread();
    return curr ? curr->process : NULL;
}

bool process_can_replace_image(const process_t *proc) {
    return proc && proc->main_thread == current_thread() && proc->live_threads == 1;
}

uint32_t process_thread_self(void) {
    thread_t *curr = current_thread();
    return curr && curr->process ? curr->tid : 0;
}

uint32_t process_active_count(void) {
    uint64_t flags;
    spin_lock_irqsave(&g_process_lock, &flags);
    uint32_t count = 0;
    for (unsigned i = 0; i < PROCESS_MAX_COUNT; i++) {
        if (g_process_pool[i].state == PROC_STATE_ALIVE) count++;
    }
    spin_unlock_irqrestore(&g_process_lock, flags);
    return count;
}

int process_info(uint64_t slot, strat_process_info_t *info) {
    if (slot >= PROCESS_MAX_COUNT) return -STRATUM_ERANGE;
    uint64_t flags;
    spin_lock_irqsave(&g_process_lock, &flags);
    const process_t *proc = &g_process_pool[slot];
    bool used = proc->state == PROC_STATE_ALIVE || proc->state == PROC_STATE_ZOMBIE;
    if (used) {
        info->pid = proc->pid;
        info->ppid = proc->ppid;
        info->exited = proc->state == PROC_STATE_ZOMBIE;
        memcpy(info->name, proc->name, sizeof(info->name));
    }
    spin_unlock_irqrestore(&g_process_lock, flags);
    return used;
}

void process_set_name(process_t *proc, const char *name) {
    uint64_t flags;
    spin_lock_irqsave(&g_process_lock, &flags);
    strncpy(proc->name, name, sizeof(proc->name) - 1);
    proc->name[sizeof(proc->name) - 1] = 0;
    spin_unlock_irqrestore(&g_process_lock, flags);
}

process_t *process_find(uint32_t pid) {
    uint64_t flags;
    spin_lock_irqsave(&g_process_lock, &flags);
    for (int i = 0; i < PROCESS_MAX_COUNT; i++) {
        if (g_process_pool[i].state != PROC_STATE_UNUSED && g_process_pool[i].pid == pid) {
            spin_unlock_irqrestore(&g_process_lock, flags);
            return &g_process_pool[i];
        }
    }
    spin_unlock_irqrestore(&g_process_lock, flags);
    return NULL;
}

static void user_process_starter(void *arg) {
    user_program_t *prog = (user_program_t *)arg;
    user_enter(prog->entry_point, prog->user_stack_top);
}

process_t *process_create_init(user_program_t *prog) {
    if (!prog) return NULL;

    uint64_t flags;
    spin_lock_irqsave(&g_process_lock, &flags);

    process_t *proc = &g_process_pool[0];
    memset(proc, 0, sizeof(process_t));
    proc->pid = 1;
    g_next_pid = 2;
    proc->ppid = 0;
    proc->state = PROC_STATE_ALIVE;
    strncpy(proc->name, "init", sizeof(proc->name) - 1);
    proc->address_space = prog->address_space;
    proc->live_threads = 1;
    spin_lock_init(&proc->lock);
    wait_queue_init(&proc->wait_child);
    wait_queue_init(&proc->wait_threads);

    fd_table_init(&proc->fds);
    file_t *c0 = file_create_console();
    file_t *c1 = file_create_console();
    file_t *c2 = file_create_console();
    if (!c0 || !c1 || !c2) {
        file_close(c0);
        file_close(c1);
        file_close(c2);
        proc->state = PROC_STATE_UNUSED;
        spin_unlock_irqrestore(&g_process_lock, flags);
        return NULL;
    }
    fd_install(&proc->fds, 0, c0);
    fd_install(&proc->fds, 1, c1);
    fd_install(&proc->fds, 2, c2);

    spin_unlock_irqrestore(&g_process_lock, flags);

    thread_t *t = thread_create_user("init", user_process_starter, prog,
                                      THREAD_PRIO_NORMAL, prog->address_space, proc, NULL, -1);
    if (!t) {
        fd_table_destroy(&proc->fds);
        proc->state = PROC_STATE_UNUSED;
        return NULL;
    }

    return proc;
}

static void fork_child_entry(void *arg) {
    syscall_regs_t regs = *(syscall_regs_t *)arg;
    kfree(arg);
    if (thread_cancelled()) process_exit_thread(-STRATUM_EINTR);
    user_enter_regs(&regs);
}

int32_t process_fork(syscall_regs_t *regs, int fixed_cpu) {
    if (!regs) return -STRATUM_EINVAL;

    process_t *parent = process_get_current();
    if (!parent) return -STRATUM_ESRCH;
    if (!process_can_replace_image(parent)) return -STRATUM_EBUSY;

    uint64_t flags;
    spin_lock_irqsave(&g_process_lock, &flags);

    process_t *child = NULL;
    for (int i = 0; i < PROCESS_MAX_COUNT; i++) {
        if (g_process_pool[i].state == PROC_STATE_UNUSED) {
            child = &g_process_pool[i];
            break;
        }
    }

    if (!child) {
        spin_unlock_irqrestore(&g_process_lock, flags);
        return -STRATUM_EAGAIN;
    }

    memset(child, 0, sizeof(process_t));
    child->pid = g_next_pid++;
    child->ppid = parent->pid;
    child->state = PROC_STATE_ALIVE;
    child->parent = parent;
    child->live_threads = 1;
    snprintf(child->name, sizeof(child->name), "proc_%u", child->pid);
    spin_lock_init(&child->lock);
    wait_queue_init(&child->wait_child);
    wait_queue_init(&child->wait_threads);

    /* Clone virtual address space with Copy-On-Write */
    child->address_space = vmm_clone_address_space(parent->address_space);
    if (!child->address_space) {
        child->state = PROC_STATE_UNUSED;
        spin_unlock_irqrestore(&g_process_lock, flags);
        return -STRATUM_ENOMEM;
    }

    memcpy(child->regions, parent->regions, sizeof(child->regions));

    /* Duplicate file descriptors */
    fd_table_copy(&child->fds, &parent->fds);

    syscall_regs_t *child_regs = kmalloc(sizeof(*child_regs));
    if (child_regs) {
        *child_regs = *regs;
        child_regs->rax = 0;
        if (thread_create_user(child->name, fork_child_entry, child_regs,
                               THREAD_PRIO_NORMAL, child->address_space, child, current_thread(), fixed_cpu)) {
            child->next_sibling = parent->children;
            parent->children = child;
            int32_t pid = (int32_t)child->pid;
            spin_unlock_irqrestore(&g_process_lock, flags);
            return pid;
        }
        kfree(child_regs);
    }
    fd_table_destroy(&child->fds);
    vmm_destroy_address_space(child->address_space);
    child->state = PROC_STATE_UNUSED;
    spin_unlock_irqrestore(&g_process_lock, flags);
    return -STRATUM_ENOMEM;
}

static uint64_t worker_stack_top(unsigned slot) {
    /* Keep an unmapped page between neighboring stacks and below the main stack. */
    return USER_STACK_TOP - (USER_STACK_PAGES + 1 + (slot - 1) * (WORKER_STACK_PAGES + 1)) * PAGE_SIZE;
}

static void release_worker_stack(process_t *proc, unsigned slot) {
    uint64_t top = worker_stack_top(slot);
    uint64_t start = top - WORKER_STACK_PAGES * PAGE_SIZE;
    futex_unmap(proc, start, top);
    for (uint64_t va = start; va < top; va += PAGE_SIZE) {
        uint64_t phys;
        if (vmm_get_mapping(proc->address_space, va, &phys, NULL) != 0) continue;
        kassert(vmm_unmap_page(proc->address_space, va) == 0);
        pmm_page_release(phys);
    }
}

static bool executable_at(process_t *proc, uint64_t address) {
    uint64_t flags;
    return address < USER_SPACE_TOP && !vmm_get_mapping(proc->address_space, address, NULL, &flags)
        && (flags & PTE_USER) && !(flags & PTE_NX);
}

int64_t process_thread_create(uint64_t trampoline, uint64_t entry, uint64_t arg, uint64_t fs_base) {
    process_t *proc = process_get_current();
    if (!proc) return -STRATUM_ESRCH;
    if (fs_base >= USER_SPACE_TOP) return -STRATUM_EINVAL;
    if (!executable_at(proc, trampoline) || !executable_at(proc, entry)) return -STRATUM_EFAULT;

    unsigned slot = 0;
    while (slot < PROCESS_WORKERS && proc->workers[slot].tid) slot++;
    if (slot == PROCESS_WORKERS) return -STRATUM_EAGAIN;

    syscall_regs_t *regs = kzalloc(sizeof(*regs));
    if (!regs) return -STRATUM_ENOMEM;

    uint64_t top = worker_stack_top(slot + 1);
    for (uint64_t va = top - WORKER_STACK_PAGES * PAGE_SIZE; va < top; va += PAGE_SIZE) {
        uint64_t phys = pmm_alloc_page();
        if (!phys) goto failed;
        memset(phys_to_virt(phys), 0, PAGE_SIZE);
        if (vmm_map_page(proc->address_space, va, phys, PTE_USER | PTE_WRITABLE | PTE_NX)) {
            pmm_free_page(phys);
            goto failed;
        }
    }
    *regs = (syscall_regs_t){
        .rip = trampoline, .rdi = entry, .rsi = arg, .rsp = top,
        .cs = 0x23, .ss = 0x1B, .rflags = 0x202,
    };
    /* The worker is pinned to this CPU and cannot run before syscall return. */
    thread_t *thread = thread_create_worker(proc, slot + 1, fs_base, fork_child_entry, regs);
    if (!thread) goto failed;
    proc->workers[slot].thread = thread;
    proc->workers[slot].tid = thread->tid;
    proc->live_threads++;
    return thread->tid;

failed:
    release_worker_stack(proc, slot + 1);
    kfree(regs);
    return -STRATUM_ENOMEM;
}

int process_thread_join(uint64_t tid, int *status) {
    process_t *proc = process_get_current();
    if (!proc) return -STRATUM_ESRCH;
    if (tid == process_thread_self()) return -STRATUM_EDEADLK;
    if (!tid || tid > UINT32_MAX) return -STRATUM_ESRCH;

    for (;;) {
        uint64_t flags;
        spin_lock_irqsave(&proc->lock, &flags);
        user_thread_slot_t *target = NULL;
        for (unsigned i = 0; i < PROCESS_WORKERS; i++) {
            if (proc->workers[i].tid == tid) {
                target = &proc->workers[i];
                break;
            }
        }
        if (!target) {
            spin_unlock_irqrestore(&proc->lock, flags);
            return -STRATUM_ESRCH;
        }
        if (!target->finished) {
            int result = wait_queue_wait_locked(&proc->wait_threads, &proc->lock, flags);
            if (result) return result;
            continue;
        }
        if (status && copy_to_user(status, &target->status, sizeof(*status))) {
            spin_unlock_irqrestore(&proc->lock, flags);
            return -STRATUM_EFAULT;
        }
        *target = (user_thread_slot_t){0};
        spin_unlock_irqrestore(&proc->lock, flags);
        return 0;
    }
}

int process_arch_prctl(uint64_t op, uint64_t value) {
    thread_t *thread = current_thread();
    if (!thread || !thread->process) return -STRATUM_ESRCH;
    if (op == ARCH_SET_FS) {
        if (value >= USER_SPACE_TOP) return -STRATUM_EINVAL;
        thread->fs_base = value;
        wrmsr(MSR_FS_BASE, value);
        return 0;
    }
    if (op == ARCH_GET_FS) {
        uint64_t base = rdmsr(MSR_FS_BASE);
        return copy_to_user((void *)value, &base, sizeof(base));
    }
    return -STRATUM_EINVAL;
}

int process_thread_cancel(uint64_t tid) {
    process_t *proc = process_get_current();
    if (!proc) return -STRATUM_ESRCH;
    if (tid == process_thread_self()) return -STRATUM_EINVAL;
    for (unsigned i = 0; i < PROCESS_WORKERS; i++) {
        const user_thread_slot_t *slot = &proc->workers[i];
        if (slot->thread && slot->tid == tid) {
            thread_cancel(slot->thread);
            return 0;
        }
    }
    return -STRATUM_ESRCH;
}

void process_exit(int status) {
    thread_t *thread = current_thread();
    process_t *proc = thread->process;
    if (proc && !proc->exiting) {
        proc->exiting = true;
        proc->exit_code = status;
        if (proc->main_thread && proc->main_thread != thread) thread_cancel(proc->main_thread);
        for (unsigned i = 0; i < PROCESS_WORKERS; i++) {
            thread_t *worker = proc->workers[i].thread;
            if (worker && worker != thread) thread_cancel(worker);
        }
    }
    process_exit_thread(status);
}

void process_exit_thread(int status) {
    thread_t *thread = current_thread();
    thread->exit_status = status;
    if (thread->process && thread->process->live_threads == 1) fd_table_destroy(&thread->process->fds);
    thread_exit();
    __builtin_unreachable();
}

static void process_finish_exit(process_t *proc) {
    uint64_t flags;
    spin_lock_irqsave(&g_process_lock, &flags);
    vmm_destroy_address_space(proc->address_space);
    proc->address_space = NULL;

    process_t *init = &g_process_pool[0];
    while (proc->children) {
        process_t *child = proc->children;
        proc->children = child->next_sibling;
        child->parent = init;
        child->ppid = init->pid;
        child->next_sibling = NULL;
        child->orphaned = true;
        if (child->state == PROC_STATE_ZOMBIE) child->state = PROC_STATE_UNUSED;
    }
    proc->state = proc->orphaned ? PROC_STATE_UNUSED : PROC_STATE_ZOMBIE;
    if (proc->parent && !proc->orphaned) wait_queue_wake_all(&proc->parent->wait_child);
    if (proc == init) {
        g_user_exit_code = proc->exit_code;
        __atomic_store_n(&g_user_init_finished, true, __ATOMIC_RELEASE);
    }
    spin_unlock_irqrestore(&g_process_lock, flags);
}

void process_finish_thread(process_t *proc, unsigned slot, int status) {
    /* The outgoing thread no longer uses either stack. Shared address spaces
       are pinned to this CPU, so no remote TLB can retain the worker mapping. */
    if (slot) {
        release_worker_stack(proc, slot);
        proc->workers[slot - 1].thread = NULL;
        proc->workers[slot - 1].status = status;
        proc->workers[slot - 1].finished = true;
        wait_queue_wake_all(&proc->wait_threads);
    } else {
        proc->main_thread = NULL;
        if (!proc->exiting) proc->exit_code = status;
    }

    if (--proc->live_threads == 0) process_finish_exit(proc);
}

int32_t process_waitpid(int32_t pid, int *status, int options) {
    process_t *parent = process_get_current();
    if (!parent) return -STRATUM_ESRCH;

    while (1) {
        uint64_t flags;
        spin_lock_irqsave(&g_process_lock, &flags);

        bool has_matching_child = false;
        process_t *prev = NULL;
        process_t *cur = parent->children;

        while (cur) {
            if (pid == -1 || pid == (int32_t)cur->pid) {
                has_matching_child = true;

                if (cur->state == PROC_STATE_ZOMBIE) {
                    /* Child reaped */
                    int exit_val = cur->exit_code;
                    uint32_t reaped_pid = cur->pid;

                    if (status && copy_to_user(status, &exit_val, sizeof(exit_val)) != 0) {
                        spin_unlock_irqrestore(&g_process_lock, flags);
                        return -STRATUM_EFAULT;
                    }

                    /* Unlink from parent */
                    if (prev) {
                        prev->next_sibling = cur->next_sibling;
                    } else {
                        parent->children = cur->next_sibling;
                    }

                    cur->state = PROC_STATE_UNUSED;
                    spin_unlock_irqrestore(&g_process_lock, flags);
                    return (int32_t)reaped_pid;
                }
            }
            prev = cur;
            cur = cur->next_sibling;
        }

        if (!has_matching_child) {
            spin_unlock_irqrestore(&g_process_lock, flags);
            return -STRATUM_ECHILD;
        }

        if (options & WNOHANG) {
            spin_unlock_irqrestore(&g_process_lock, flags);
            return 0;
        }

        int result = wait_queue_wait_locked(&parent->wait_child, &g_process_lock, flags);
        if (result) return result;
    }
}
