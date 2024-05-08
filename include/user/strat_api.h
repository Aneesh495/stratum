#ifndef STRAT_API_H
#define STRAT_API_H

#include <stdint.h>
#include <stddef.h>
#include <shared/syscall_nums.h>

#define POLLIN      0x0001
#define POLLPRI     0x0002
#define POLLOUT     0x0004
#define POLLERR     0x0008
#define POLLHUP     0x0010
#define POLLNVAL    0x0020

struct pollfd {
    int      fd;
    int16_t  events;
    int16_t  revents;
};

struct in_addr {
    uint32_t s_addr;
};

struct sockaddr {
    uint16_t sa_family;
    char     sa_data[14];
};

struct sockaddr_in {
    uint16_t       sin_family;
    uint16_t       sin_port;
    struct in_addr sin_addr;
    uint8_t        sin_zero[8];
};

typedef struct {
    uint64_t st_ino;
    uint32_t st_mode;
    uint32_t st_nlink;
    uint32_t st_uid;
    uint32_t st_gid;
    uint64_t st_size;
    uint32_t st_blksize;
    uint64_t st_blocks;
    uint64_t st_atime;
    uint64_t st_mtime;
    uint64_t st_ctime;
} user_stat_t;

static inline int64_t syscall0(uint64_t nr) {
    int64_t ret;
    __asm__ volatile("syscall" : "=a"(ret) : "a"(nr) : "rcx", "r11", "memory");
    return ret;
}

static inline int64_t syscall1(uint64_t nr, uint64_t a1) {
    int64_t ret;
    __asm__ volatile("syscall" : "=a"(ret) : "a"(nr), "D"(a1) : "rcx", "r11", "memory");
    return ret;
}

static inline int64_t syscall2(uint64_t nr, uint64_t a1, uint64_t a2) {
    int64_t ret;
    __asm__ volatile("syscall" : "=a"(ret) : "a"(nr), "D"(a1), "S"(a2) : "rcx", "r11", "memory");
    return ret;
}

static inline int64_t syscall3(uint64_t nr, uint64_t a1, uint64_t a2, uint64_t a3) {
    int64_t ret;
    __asm__ volatile("syscall" : "=a"(ret) : "a"(nr), "D"(a1), "S"(a2), "d"(a3) : "rcx", "r11", "memory");
    return ret;
}

static inline int64_t syscall4(uint64_t nr, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    int64_t ret;
    register uint64_t r10 __asm__("r10") = a4;
    __asm__ volatile("syscall" : "=a"(ret) : "a"(nr), "D"(a1), "S"(a2), "d"(a3), "r"(r10) : "rcx", "r11", "memory");
    return ret;
}

static inline int64_t syscall5(uint64_t nr, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    int64_t ret;
    register uint64_t r10 __asm__("r10") = a4;
    register uint64_t r8  __asm__("r8")  = a5;
    __asm__ volatile("syscall" : "=a"(ret) : "a"(nr), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8) : "rcx", "r11", "memory");
    return ret;
}

/* Anonymous mappings return an address or a negative Stratum errno. */
static inline int64_t mmap_anonymous(size_t length, unsigned protection) {
    int64_t result;
    register uint64_t r10 __asm__("r10") = MAP_PRIVATE | MAP_ANONYMOUS;
    register uint64_t r8 __asm__("r8") = (uint64_t)-1;
    register uint64_t r9 __asm__("r9") = 0;
    __asm__ volatile("syscall" : "=a"(result)
        : "a"(SYS_mmap), "D"(0), "S"(length), "d"(protection), "r"(r10), "r"(r8), "r"(r9)
        : "rcx", "r11", "memory");
    return result;
}

static inline int munmap(void *addr, size_t length) {
    return (int)syscall2(SYS_munmap, (uint64_t)addr, length);
}

static inline int mprotect(void *addr, size_t length, unsigned protection) {
    return (int)syscall3(SYS_mprotect, (uint64_t)addr, length, protection);
}

static inline int execve(const char *path, const char *const *argv, const char *const *envp) {
    return (int)syscall3(SYS_execve, (uint64_t)path, (uint64_t)argv, (uint64_t)envp);
}

void strat_thread_start(void);

static inline int64_t strat_thread_create(int (*entry)(void *), void *arg, void *tls) {
    return syscall4(SYS_thread_create, (uintptr_t)strat_thread_start, (uintptr_t)entry,
                    (uintptr_t)arg, (uintptr_t)tls);
}

static inline int strat_thread_join(uint32_t tid, int *status) {
    return (int)syscall2(SYS_thread_join, tid, (uintptr_t)status);
}

static inline uint32_t strat_thread_self(void) {
    return (uint32_t)syscall0(SYS_thread_self);
}

static inline int strat_futex_wait(uint32_t *word, uint32_t expected, uint32_t timeout_ms) {
    return (int)syscall4(SYS_futex, (uintptr_t)word, FUTEX_WAIT, expected, timeout_ms);
}

static inline int strat_futex_wake(uint32_t *word, uint32_t count) {
    return (int)syscall4(SYS_futex, (uintptr_t)word, FUTEX_WAKE, count, 0);
}

static inline int strat_set_fs(void *base) {
    return (int)syscall2(SYS_arch_prctl, ARCH_SET_FS, (uintptr_t)base);
}

static inline int strat_get_fs(uintptr_t *base) {
    return (int)syscall2(SYS_arch_prctl, ARCH_GET_FS, (uintptr_t)base);
}

void exit(int status) __attribute__((noreturn));

/* File System API */
static inline int write(int fd, const void *buf, size_t count) {
    return (int)syscall3(SYS_write, (uint64_t)fd, (uint64_t)buf, (uint64_t)count);
}

static inline int read(int fd, void *buf, size_t count) {
    return (int)syscall3(SYS_read, (uint64_t)fd, (uint64_t)buf, (uint64_t)count);
}

static inline int open(const char *path, int flags, int mode) {
    return (int)syscall3(SYS_open, (uint64_t)path, (uint64_t)flags, (uint64_t)mode);
}

static inline int rename(const char *old_path, const char *new_path) {
    return (int)syscall2(SYS_rename, (uint64_t)old_path, (uint64_t)new_path);
}

static inline int readdir(int fd, strat_dirent_t *entry) {
    return (int)syscall2(SYS_readdir, (uint64_t)fd, (uint64_t)entry);
}

static inline int close(int fd) {
    return (int)syscall1(SYS_close, (uint64_t)fd);
}

static inline int64_t lseek(int fd, int64_t offset, int whence) {
    return syscall3(SYS_lseek, (uint64_t)fd, (uint64_t)offset, (uint64_t)whence);
}

static inline int stat(const char *path, user_stat_t *st) {
    return (int)syscall2(SYS_stat, (uint64_t)path, (uint64_t)st);
}

static inline int mkdir(const char *path, int mode) {
    return (int)syscall2(SYS_mkdir, (uint64_t)path, (uint64_t)mode);
}

static inline int unlink(const char *path) {
    return (int)syscall1(SYS_unlink, (uint64_t)path);
}

static inline int fsync(int fd) {
    return (int)syscall1(SYS_fsync, (uint64_t)fd);
}

static inline int dup2(int oldfd, int newfd) {
    return (int)syscall2(SYS_dup2, (uint64_t)oldfd, (uint64_t)newfd);
}

static inline int pipe(int pipefd[2]) {
    return (int)syscall1(SYS_pipe, (uint64_t)pipefd);
}

static inline int pipe2(int descriptors[2], unsigned flags) {
    return (int)syscall2(SYS_pipe2, (uintptr_t)descriptors, flags);
}

static inline int strat_channel_pair(int descriptors[2], unsigned flags) {
    return (int)syscall2(SYS_channel_pair, (uintptr_t)descriptors, flags);
}

static inline int poll(struct pollfd *fds, uint32_t nfds, int timeout) {
    return (int)syscall3(SYS_poll, (uint64_t)fds, (uint64_t)nfds, (uint64_t)timeout);
}

/* Socket API */
static inline int socket(int domain, int type, int protocol) {
    return (int)syscall3(SYS_socket, (uint64_t)domain, (uint64_t)type, (uint64_t)protocol);
}

static inline int bind(int sockfd, const struct sockaddr *addr, uint32_t addrlen) {
    return (int)syscall3(SYS_bind, (uint64_t)sockfd, (uint64_t)addr, (uint64_t)addrlen);
}

static inline int listen(int sockfd, int backlog) {
    return (int)syscall2(SYS_listen, (uint64_t)sockfd, (uint64_t)backlog);
}

static inline int accept(int sockfd, struct sockaddr *addr, uint32_t *addrlen) {
    return (int)syscall3(SYS_accept, (uint64_t)sockfd, (uint64_t)addr, (uint64_t)addrlen);
}

static inline int connect(int sockfd, const struct sockaddr *addr, uint32_t addrlen) {
    return (int)syscall3(SYS_connect, (uint64_t)sockfd, (uint64_t)addr, (uint64_t)addrlen);
}

static inline int64_t send(int sockfd, const void *buf, size_t len, int flags) {
    return syscall4(SYS_send, (uint64_t)sockfd, (uint64_t)buf, (uint64_t)len, (uint64_t)flags);
}

static inline int64_t recv(int sockfd, void *buf, size_t len, int flags) {
    return syscall4(SYS_recv, (uint64_t)sockfd, (uint64_t)buf, (uint64_t)len, (uint64_t)flags);
}

static inline int shutdown(int sockfd, int how) {
    return (int)syscall2(SYS_shutdown, (uint64_t)sockfd, (uint64_t)how);
}

/* Process and System Management */
static inline int fork(void) {
    return (int)syscall0(SYS_fork);
}

static inline int strat_fork_on_cpu(unsigned cpu) {
    return (int)syscall1(SYS_fork_on_cpu, cpu);
}

static inline int waitpid(int pid, int *status, int options) {
    return (int)syscall3(SYS_waitpid, (uint64_t)pid, (uint64_t)status, (uint64_t)options);
}

static inline int getpid(void) {
    return (int)syscall0(SYS_getpid);
}

static inline int yield(void) {
    return (int)syscall0(SYS_sched_yield);
}

static inline int strat_getcpu(void) {
    return (int)syscall0(SYS_getcpu);
}

static inline int strat_net_queues(strat_queue_stats_t queues[2]) {
    return (int)syscall1(SYS_net_queues, (uintptr_t)queues);
}

static inline int64_t strat_sched_switches(unsigned cpu) {
    return syscall1(SYS_sched_switches, cpu);
}

static inline int sleep_ms(uint32_t ms) {
    return (int)syscall1(SYS_nanosleep, (uint64_t)ms);
}

static inline uint64_t uptime(void) {
    return (uint64_t)syscall0(SYS_clock_gettime);
}

static inline int sysinfo(strat_sysinfo_t *info) {
    return (int)syscall1(SYS_sysinfo, (uint64_t)info);
}

static inline void trace_emit(uint32_t event_id, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    syscall5(SYS_trace_emit, (uint64_t)event_id, a1, a2, a3, a4);
}

#endif /* STRAT_API_H */
