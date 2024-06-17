#ifndef STRATUM_SHARED_SYSCALL_NUMS_H
#define STRATUM_SHARED_SYSCALL_NUMS_H

#define SYS_exit            1
#define SYS_fork            2
#define SYS_execve          3
#define SYS_waitpid         4
#define SYS_getpid          5

#define SYS_thread_create   10
#define SYS_thread_exit     11
#define SYS_thread_join     12
#define SYS_thread_self     13
#define SYS_futex           14
#define SYS_arch_prctl      15
#define SYS_sched_yield     16
#define SYS_getcpu          17
#define SYS_fork_on_cpu     18
#define SYS_thread_cancel   19

#define SYS_mmap            20
#define SYS_munmap          21
#define SYS_mprotect        22
#define SYS_brk             23

#define SYS_read            30
#define SYS_write           31
#define SYS_open            32
#define SYS_close           33
#define SYS_lseek           34
#define SYS_dup2            35
#define SYS_pipe            36
#define SYS_poll            37
#define SYS_pipe2           38
#define SYS_channel_pair    39

#define SYS_stat            40
#define SYS_mkdir           41
#define SYS_unlink          42
#define SYS_rename          43
#define SYS_readdir         44
#define SYS_fsync           45
#define SYS_rmdir           46
#define SYS_link            47
#define SYS_symlink         48
#define SYS_readlink        49

#define SYS_socket          50
#define SYS_bind            51
#define SYS_listen          52
#define SYS_accept          53
#define SYS_connect         54
#define SYS_send            55
#define SYS_recv            56
#define SYS_shutdown        57

#define SYS_clock_gettime   60
#define SYS_nanosleep       61

#define SYS_trace_emit      70
#define SYS_sysinfo         71
#define SYS_sched_switches  72
#define SYS_net_queues      73
#define SYS_net_rx_irqs     74
#define SYS_resources       75
#define SYS_fs_cache        76
#define SYS_blk_irq_wakes   77
#define SYS_trace_info      78
#define SYS_trace_read      79
#define SYS_tcp_recovery    80

#define STRAT_TRACE_USER_BASE         100

#define STRAT_CHANNEL_MAX_MESSAGE     512
#define STRAT_CHANNEL_QUEUE_CAPACITY  8

/* Futex operations */
#define FUTEX_WAIT          0
#define FUTEX_WAKE          1
#define FUTEX_REQUEUE       2

/* Arch prctl operations */
#define ARCH_SET_FS         0x1002
#define ARCH_GET_FS         0x1003
#define ARCH_SET_GS         0x1004
#define ARCH_GET_GS         0x1005

/* Open flags */
#define O_RDONLY            0x0000
#define O_WRONLY            0x0001
#define O_RDWR              0x0002
#define O_CREAT             0x0040
#define O_EXCL              0x0080
#define O_TRUNC             0x0200
#define O_APPEND            0x0400
#define O_NONBLOCK          0x0800
#define O_DIRECTORY         0x10000
#define O_CLOEXEC           0x80000

/* Seek whence */
#define SEEK_SET            0
#define SEEK_CUR            1
#define SEEK_END            2

/* Poll events */
#define POLLIN              0x0001
#define POLLPRI             0x0002
#define POLLOUT             0x0004
#define POLLERR             0x0008
#define POLLHUP             0x0010
#define POLLNVAL            0x0020

/* Memory protection */
#define PROT_NONE           0x0
#define PROT_READ           0x1
#define PROT_WRITE          0x2
#define PROT_EXEC           0x4

/* Memory map flags */
#define MAP_SHARED          0x01
#define MAP_PRIVATE         0x02
#define MAP_FIXED           0x10
#define MAP_ANONYMOUS       0x20

/* Socket domains and types */
#define AF_INET             2
#define SOCK_STREAM         1
#define SOCK_DGRAM          2
#define IPPROTO_TCP         6
#define IPPROTO_UDP         17

/* Socket shutdown flags */
#define SHUT_RD             0
#define SHUT_WR             1
#define SHUT_RDWR           2

#ifndef __ASSEMBLER__
typedef struct strat_dirent {
    uint64_t d_ino;
    uint8_t d_type;
    char d_name[64];
} strat_dirent_t;

typedef struct strat_sysinfo {
    uint64_t uptime_ms;
    uint32_t online_cpus;
    uint32_t active_processes;
    uint64_t total_memory_bytes;
    uint64_t free_memory_bytes;
    uint64_t total_syscalls;
} strat_sysinfo_t;

typedef struct strat_trace_info {
    uint64_t first;
    uint64_t next;
    uint64_t lost;
} strat_trace_info_t;

typedef struct strat_trace_record {
    uint64_t tsc;
    uint32_t cpu;
    uint32_t event;
    uint64_t args[4];
} strat_trace_record_t;

typedef struct strat_cache_stats {
    uint64_t hits;
    uint64_t misses;
    uint64_t evictions;
    uint64_t invalidations;
} strat_cache_stats_t;

typedef struct strat_resources {
    uint64_t threads;
    uint64_t file_objects;
    uint64_t packet_buffers;
    uint32_t journal_capacity;
    uint32_t journal_pending;
} strat_resources_t;

typedef struct strat_queue_stats {
    uint64_t submitted;
    uint64_t completed;
    uint32_t capacity;
    uint32_t free;
    uint16_t available_idx;
    uint16_t used_idx;
    uint32_t failed;
} strat_queue_stats_t;

#endif /* __ASSEMBLER__ */

#endif /* STRATUM_SHARED_SYSCALL_NUMS_H */
