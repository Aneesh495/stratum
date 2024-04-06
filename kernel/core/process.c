#include <kernel/process.h>
#include <kernel/slab.h>
#include <kernel/kernel.h>
#include <kernel/string.h>
#include <kernel/smp.h>
#include <kernel/vmm.h>
#include <shared/errno.h>

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

process_t *process_get_current(void) {
    uint32_t cpu_id = smp_get_cpu_id();
    thread_t *curr = g_runqueues[cpu_id].current_thread;
    return curr ? curr->process : NULL;
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
    spin_lock_init(&proc->lock);
    wait_queue_init(&proc->wait_child);

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
                                      THREAD_PRIO_NORMAL, prog->address_space, proc);
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
    user_enter_regs(&regs);
}

int32_t process_fork(syscall_regs_t *regs) {
    if (!regs) return -STRATUM_EINVAL;

    process_t *parent = process_get_current();
    if (!parent) return -STRATUM_ESRCH;

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
    snprintf(child->name, sizeof(child->name), "proc_%u", child->pid);
    spin_lock_init(&child->lock);
    wait_queue_init(&child->wait_child);

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
                               THREAD_PRIO_NORMAL, child->address_space, child)) {
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

void process_exit(int status) {
    process_t *proc = process_get_current();
    if (proc) {
        proc->exit_code = status;
        fd_table_destroy(&proc->fds);
    }
    thread_exit();
    __builtin_unreachable();
}

void process_finish_exit(process_t *proc) {
    /* The outgoing thread's stack and page tables are no longer active. */
    uint64_t flags;
    spin_lock_irqsave(&g_process_lock, &flags);
    vmm_destroy_address_space(proc->address_space);
    proc->address_space = NULL;
    proc->main_thread = NULL;

    process_t *init = &g_process_pool[0];
    while (proc != init && proc->children) {
        process_t *child = proc->children;
        proc->children = child->next_sibling;
        child->parent = init;
        child->ppid = init->pid;
        child->next_sibling = init->children;
        init->children = child;
    }
    proc->state = PROC_STATE_ZOMBIE;
    if (proc->parent) wait_queue_wake_all(&proc->parent->wait_child);
    if (proc == init) {
        g_user_exit_code = proc->exit_code;
        __atomic_store_n(&g_user_init_finished, true, __ATOMIC_RELEASE);
    }
    spin_unlock_irqrestore(&g_process_lock, flags);
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

        wait_queue_wait_locked(&parent->wait_child, &g_process_lock, flags);
    }
}
