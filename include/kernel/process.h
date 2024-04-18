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

#define PROCESS_MAX_COUNT 64
#define PROCESS_ANON_REGIONS 16

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
} user_region_t;

typedef struct process {
    uint32_t         pid;
    uint32_t         ppid;
    char             name[32];
    process_state_t  state;
    pml4_t          *address_space;
    thread_t        *main_thread;
    user_region_t    regions[PROCESS_ANON_REGIONS];
    fd_table_t       fds;
    int              exit_code;

    struct process  *parent;
    struct process  *children;
    struct process  *next_sibling;

    wait_queue_t     wait_child;
    spinlock_t       lock;
} process_t;

void       process_init(void);
process_t *process_get_current(void);
uint32_t   process_active_count(void);
process_t *process_find(uint32_t pid);
process_t *process_create_init(user_program_t *prog);
int        process_exec(const char *path, const char *const *argv, const char *const *envp);
int32_t    process_fork(syscall_regs_t *regs);
void       process_exit(int status) __attribute__((noreturn));
void       process_finish_exit(process_t *proc);
int32_t    process_waitpid(int32_t pid, int *status, int options);

int64_t    user_map_anonymous(process_t *process, uint64_t length, unsigned protection);
int        user_unmap(process_t *process, uint64_t start, uint64_t length);
int        user_protect(process_t *process, uint64_t start, uint64_t length, unsigned protection);
bool       user_demand_page(process_t *process, uint64_t address, bool write, bool execute);

#endif /* STRATUM_KERNEL_PROCESS_H */
