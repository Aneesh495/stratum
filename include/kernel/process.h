#ifndef STRATUM_KERNEL_PROCESS_H
#define STRATUM_KERNEL_PROCESS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <kernel/spinlock.h>
#include <kernel/sched.h>
#include <kernel/file.h>
#include <kernel/vmm.h>
#include <kernel/syscall.h>
#include <kernel/user_elf.h>
#include <shared/syscall_nums.h>

#define PROCESS_MAX_COUNT STRAT_PROCESS_SLOTS
#define PROCESS_ANON_REGIONS 16
#define PROCESS_WORKERS 32

#define WNOHANG     1
#define WUNTRACED   2

typedef enum {
    PROC_STATE_UNUSED = 0,
    PROC_STATE_ALIVE,
    PROC_STATE_ZOMBIE,
    PROC_STATE_DEAD
} process_state_t;

typedef struct user_region {
    uint64_t start;
    uint64_t length;
    unsigned protection;
    bool shared;
} user_region_t;

typedef struct {
    thread_t *thread;
    uint32_t tid;
    int status;
    bool finished;
} user_thread_slot_t;

typedef struct process {
    uint32_t         pid;
    uint32_t         ppid;
    char             name[STRAT_PROCESS_NAME];
    process_state_t  state;
    pml4_t          *address_space;
    thread_t        *main_thread;
    unsigned         live_threads;
    user_thread_slot_t workers[PROCESS_WORKERS];
    wait_queue_t     wait_threads;
    user_region_t    regions[PROCESS_ANON_REGIONS];
    fd_table_t       fds;
    int              exit_code;
    bool             orphaned;
    bool             exiting;

    struct process  *parent;
    struct process  *children;
    struct process  *next_sibling;

    wait_queue_t     wait_child;
    spinlock_t       lock;
} process_t;

void       process_init(void);
process_t *process_get_current(void);
uint32_t   process_active_count(void);
int        process_info(uint64_t slot, strat_process_info_t *info);
void       process_set_name(process_t *process, const char *name);
process_t *process_find(uint32_t pid);
process_t *process_create_init(user_program_t *prog);
int        process_exec(const char *path, const char *const *argv, const char *const *envp);
int32_t    process_fork(syscall_regs_t *regs, int fixed_cpu);
void       process_exit(int status) __attribute__((noreturn));
void       process_exit_thread(int status) __attribute__((noreturn));
int        process_thread_cancel(uint64_t tid);
void       process_finish_thread(process_t *proc, unsigned slot, int status);
int64_t    process_thread_create(uint64_t trampoline, uint64_t entry, uint64_t arg, uint64_t fs_base);
int        process_thread_join(uint64_t tid, int *status);
int        process_arch_prctl(uint64_t op, uint64_t value);
uint32_t   process_thread_self(void);
bool       process_can_replace_image(const process_t *proc);
int32_t    process_waitpid(int32_t pid, int *status, int options);

int64_t    user_map_anonymous(process_t *process, uint64_t length, unsigned protection, bool shared);
int        user_unmap(process_t *process, uint64_t start, uint64_t length);
int        user_protect(process_t *process, uint64_t start, uint64_t length, unsigned protection);
bool       user_demand_page(process_t *process, uint64_t address, bool write, bool execute);

#endif /* STRATUM_KERNEL_PROCESS_H */
