#include <kernel/syscall.h>
#include <kernel/kernel.h>
#include <kernel/smp.h>
#include <kernel/sched.h>
#include <kernel/vmm.h>
#include <kernel/x86_64.h>
#include <kernel/process.h>
#include <kernel/file.h>
#include <kernel/event.h>
#include <kernel/uart.h>
#include <kernel/pipe.h>
#include <kernel/channel.h>
#include <kernel/futex.h>
#include <kernel/vfs.h>
#include <kernel/socket.h>
#include <kernel/virtio_net.h>
#include <kernel/net.h>
#include <kernel/tcp.h>
#include <kernel/stratafs.h>
#include <kernel/trace.h>
#include <kernel/pmm.h>
#include <kernel/string.h>
#include <shared/syscall_nums.h>
#include <shared/errno.h>

extern void syscall_entry(void);

static volatile uint64_t g_total_syscall_count = 0;

static inline bool is_user_range_valid(const void *addr, size_t len) {
    uint64_t start = (uint64_t)addr;
    uint64_t end = start + len;
    if (end < start) return false; /* Overflow */
    if (end > USER_SPACE_TOP) return false; /* Above canonical lower half */
    return true;
}

void syscall_init_cpu(void) {
    /* 1. Enable System Call Extensions (SCE) in EFER */
    uint64_t efer = rdmsr(MSR_EFER);
    wrmsr(MSR_EFER, efer | 1);

    /* 2. Configure STAR MSR (0xC0000081):
     * Bits 47:32 = KERNEL_CS (0x08) -> Syscall sets CS=0x08, SS=0x10
     * Bits 63:48 = USER_CS_BASE (0x10) -> Sysret sets SS=0x1B, CS=0x23
     */
    uint64_t star = ((uint64_t)0x10 << 48) | ((uint64_t)0x08 << 32);
    wrmsr(MSR_STAR, star);

    /* 3. Configure LSTAR MSR (0xC0000082): Target RIP for syscall */
    wrmsr(MSR_LSTAR, (uint64_t)syscall_entry);

    /* 4. Configure SFMASK MSR (0xC0000084):
     * Mask out IF (0x200), DF (0x400), TF (0x100), NT (0x4000), AC (0x40000)
     */
    wrmsr(MSR_SFMASK, 0x44700);

    /* 5. Set KERNEL_GS_BASE to current cpu_t pointer so swapgs works on syscall */
    cpu_t *cpu = smp_get_current_cpu();
    wrmsr(MSR_KERNEL_GS_BASE, (uint64_t)cpu);
}

void syscall_init(void) {
    syscall_init_cpu();
    kprintf("[SYSCALL] Fast syscall/sysret MSRs configured (Ring 3 user ABI active)\n");
}

volatile bool g_user_init_finished = false;
volatile int  g_user_exit_code = 0;

static int64_t sys_read_impl(int fd, void *u_buf, size_t count) {
    if (!is_user_range_valid(u_buf, count)) {
        return -STRATUM_EFAULT;
    }

    process_t *proc = process_get_current();
    if (!proc) return -STRATUM_ESRCH;

    file_t *f = fd_get(&proc->fds, fd);
    if (!f) return -STRATUM_EBADF;

    if (f->type == FILE_TYPE_CHANNEL) {
        int64_t result = channel_read_user(f, u_buf, count);
        file_close(f);
        return result;
    }

    if (!f->ops || !f->ops->read) {
        file_close(f);
        return -STRATUM_EINVAL;
    }

    char kbuf[512];
    size_t to_read = (count > sizeof(kbuf)) ? sizeof(kbuf) : count;
    int64_t res = f->ops->read(f, kbuf, to_read);

    if (res > 0) {
        if (copy_to_user(u_buf, kbuf, (size_t)res) != 0) {
            file_close(f);
            return -STRATUM_EFAULT;
        }
    }

    file_close(f);
    return res;
}

static int64_t sys_write_impl(int fd, const void *u_buf, size_t count) {
    if (!is_user_range_valid(u_buf, count)) {
        return -STRATUM_EFAULT;
    }

    process_t *proc = process_get_current();
    if (!proc) return -STRATUM_ESRCH;

    file_t *f = fd_get(&proc->fds, fd);
    if (!f) return -STRATUM_EBADF;

    if (f->type == FILE_TYPE_CHANNEL && count > STRAT_CHANNEL_MAX_MESSAGE) {
        file_close(f);
        return -STRATUM_EMSGSIZE;
    }

    if (!f->ops || !f->ops->write) {
        file_close(f);
        return -STRATUM_EINVAL;
    }

    char kbuf[512];
    size_t written = 0;
    const char *src = (const char *)u_buf;

    while (written < count) {
        size_t chunk = (count - written > sizeof(kbuf)) ? sizeof(kbuf) : (count - written);
        if (copy_from_user(kbuf, src + written, chunk) != 0) {
            file_close(f);
            return written ? (int64_t)written : -STRATUM_EFAULT;
        }

        int64_t res = f->ops->write(f, kbuf, chunk);
        if (res < 0) {
            file_close(f);
            return (written > 0) ? (int64_t)written : res;
        }
        written += (size_t)res;
        if ((size_t)res < chunk) break;
    }

    file_close(f);
    return (int64_t)written;
}

static int copy_str_from_user(char *dst_k, const char *src_u, size_t max_len) {
    if (!dst_k || !src_u || max_len == 0) return -STRATUM_EINVAL;
    size_t i = 0;
    while (i < max_len) {
        char c;
        if (copy_from_user(&c, src_u + i, 1) != 0) {
            return -STRATUM_EFAULT;
        }
        dst_k[i] = c;
        if (c == '\0') {
            return 0;
        }
        i++;
    }
    return -STRATUM_ENAMETOOLONG;
}

static int64_t sys_open_impl(const char *u_path, int flags, int mode) {
    if (!u_path) return -STRATUM_EINVAL;
    char kpath[VFS_MAX_PATH];
    memset(kpath, 0, sizeof(kpath));
    int path_result = copy_str_from_user(kpath, u_path, sizeof(kpath));
    if (path_result != 0) return path_result;

    process_t *proc = process_get_current();
    if (!proc) return -STRATUM_ESRCH;

    file_t *f = NULL;
    int res = vfs_open(kpath, flags, mode, &f);
    if (res != 0) return res;

    int fd = fd_alloc_flags(&proc->fds, f, flags & O_CLOEXEC);
    if (fd < 0) {
        vfs_close(f);
        return fd;
    }
    return fd;
}

static int64_t sys_lseek_impl(int fd, int64_t offset, int whence) {
    process_t *proc = process_get_current();
    if (!proc) return -STRATUM_ESRCH;
    file_t *f = fd_get(&proc->fds, fd);
    if (!f) return -STRATUM_EBADF;
    int64_t res = vfs_lseek(f, offset, whence);
    file_close(f);
    return res;
}

static int64_t sys_stat_impl(const char *u_path, vfs_stat_t *u_st) {
    if (!u_path || !u_st) return -STRATUM_EINVAL;
    char kpath[VFS_MAX_PATH];
    memset(kpath, 0, sizeof(kpath));
    int path_result = copy_str_from_user(kpath, u_path, sizeof(kpath));
    if (path_result != 0) return path_result;
    vfs_stat_t kst;
    int res = vfs_stat(kpath, &kst);
    if (res != 0) return res;
    if (copy_to_user(u_st, &kst, sizeof(kst)) != 0) {
        return -STRATUM_EFAULT;
    }
    return 0;
}

static int64_t sys_mkdir_impl(const char *u_path, int mode) {
    if (!u_path) return -STRATUM_EINVAL;
    char kpath[VFS_MAX_PATH];
    memset(kpath, 0, sizeof(kpath));
    int path_result = copy_str_from_user(kpath, u_path, sizeof(kpath));
    if (path_result != 0) return path_result;
    return vfs_mkdir(kpath, mode);
}

static int64_t sys_remove_impl(const char *u_path, bool directory) {
    if (!u_path) return -STRATUM_EINVAL;
    char kpath[VFS_MAX_PATH];
    memset(kpath, 0, sizeof(kpath));
    int path_result = copy_str_from_user(kpath, u_path, sizeof(kpath));
    if (path_result != 0) return path_result;
    return directory ? vfs_rmdir(kpath) : vfs_unlink(kpath);
}

static int64_t sys_move_impl(const char *old_path, const char *new_path, uint64_t operation) {
    char old_name[VFS_MAX_PATH], new_name[VFS_MAX_PATH];
    int res = copy_str_from_user(old_name, old_path, sizeof(old_name));
    if (res == 0) res = copy_str_from_user(new_name, new_path, sizeof(new_name));
    if (res) return res;
    if (operation == SYS_symlink) return vfs_symlink(old_name, new_name);
    if (operation == SYS_link) return vfs_link(old_name, new_name);
    return vfs_rename(old_name, new_name);
}

static int64_t sys_readlink_impl(const char *path, char *out, size_t size) {
    if (!size) return -STRATUM_EINVAL;
    char kpath[VFS_MAX_PATH], target[VFS_MAX_PATH];
    int result = copy_str_from_user(kpath, path, sizeof(kpath));
    if (result) return result;
    int64_t length = vfs_readlink(kpath, target, size < sizeof(target) ? size : sizeof(target));
    if (length > 0 && copy_to_user(out, target, (size_t)length) != 0) return -STRATUM_EFAULT;
    return length;
}

static int64_t sys_readdir_impl(int fd, strat_dirent_t *user_entry) {
    process_t *proc = process_get_current();
    if (!proc) return -STRATUM_ESRCH;
    file_t *file = fd_get(&proc->fds, fd);
    if (!file) return -STRATUM_EBADF;
    if (file->type != FILE_TYPE_VFS || !(file->flags & O_DIRECTORY)) {
        file_close(file);
        return -STRATUM_ENOTDIR;
    }

    vfs_node_t *node = file->priv;
    mutex_lock(&file->lock);
    uint64_t offset = file->offset;
    strat_dirent_t entry = {0};
    int res = node->ops && node->ops->readdir ? node->ops->readdir(node, &offset, &entry) : -STRATUM_ENOSYS;
    if (res > 0 && copy_to_user(user_entry, &entry, sizeof(entry)) != 0) res = -STRATUM_EFAULT;
    if (res >= 0) file->offset = offset;
    mutex_unlock(&file->lock);
    file_close(file);
    return res;
}

static int64_t sys_fsync_impl(int fd) {
    process_t *proc = process_get_current();
    if (!proc) return -STRATUM_ESRCH;
    file_t *f = fd_get(&proc->fds, fd);
    if (!f) return -STRATUM_EBADF;
    int res = -STRATUM_EINVAL;
    if (f->type == FILE_TYPE_VFS && f->priv) {
        vfs_node_t *node = (vfs_node_t *)f->priv;
        if (node->ops && node->ops->sync) {
            res = node->ops->sync(node);
        }
    }
    file_close(f);
    return res;
}

static int64_t sys_bind_user(int fd, const struct sockaddr *u_addr, uint32_t addrlen) {
    if (!u_addr || addrlen > 128) return -STRATUM_EINVAL;
    char kaddr[128];
    if (copy_from_user(kaddr, u_addr, addrlen) != 0) return -STRATUM_EFAULT;
    return sys_bind(fd, (const struct sockaddr *)kaddr, addrlen);
}

static int64_t sys_accept_user(int fd, struct sockaddr *u_addr, uint32_t *u_addrlen) {
    if (!u_addr) return sys_accept(fd, NULL, NULL);
    uint32_t supplied;
    if (!u_addrlen || copy_from_user(&supplied, u_addrlen, sizeof(supplied)) != 0) {
        return -STRATUM_EFAULT;
    }
    struct sockaddr_in peer;
    uint32_t actual = sizeof(peer);
    int cfd = sys_accept(fd, (struct sockaddr *)&peer, &actual);
    if (cfd < 0) return cfd;
    size_t n = supplied < actual ? supplied : actual;
    if (copy_to_user(u_addr, &peer, n) != 0 ||
        copy_to_user(u_addrlen, &actual, sizeof(actual)) != 0) {
        socket_close_fd(cfd);
        return -STRATUM_EFAULT;
    }
    return cfd;
}

static int64_t sys_connect_user(int fd, const struct sockaddr *u_addr, uint32_t addrlen) {
    if (!u_addr || addrlen > 128) return -STRATUM_EINVAL;
    char kaddr[128];
    if (copy_from_user(kaddr, u_addr, addrlen) != 0) return -STRATUM_EFAULT;
    return sys_connect(fd, (const struct sockaddr *)kaddr, addrlen);
}

static int64_t sys_send_user(int fd, const void *u_buf, size_t len, int flags) {
    if (!is_user_range_valid(u_buf, len)) return -STRATUM_EFAULT;
    int type = socket_fd_type(fd);
    if (type < 0) return type;
    char kbuf[UDP_MAX_PAYLOAD];
    if (type == SOCK_DGRAM && len > sizeof(kbuf)) return -STRATUM_EMSGSIZE;
    size_t n = len < sizeof(kbuf) ? len : sizeof(kbuf);
    if (copy_from_user(kbuf, u_buf, n) != 0) return -STRATUM_EFAULT;
    return sys_send(fd, kbuf, n, flags);
}

static int64_t sys_recv_user(int fd, void *u_buf, size_t len, int flags) {
    if (!is_user_range_valid(u_buf, len)) return -STRATUM_EFAULT;
    char kbuf[UDP_MAX_PAYLOAD];
    size_t n = len < sizeof(kbuf) ? len : sizeof(kbuf);
    int64_t res = sys_recv(fd, kbuf, n, flags);
    if (res > 0 && copy_to_user(u_buf, kbuf, (size_t)res) != 0) return -STRATUM_EFAULT;
    return res;
}

static int64_t sys_sysinfo_impl(strat_sysinfo_t *u_info) {
    if (!u_info || !is_user_range_valid(u_info, sizeof(strat_sysinfo_t))) return -STRATUM_EFAULT;
    strat_sysinfo_t kinfo;
    memset(&kinfo, 0, sizeof(kinfo));
    kinfo.uptime_ms = timer_get_uptime_ms();
    kinfo.online_cpus = smp_get_online_cpus();
    kinfo.active_processes = process_active_count();
    kinfo.total_memory_bytes = pmm_get_total_pages() * PAGE_SIZE;
    kinfo.free_memory_bytes = pmm_get_free_pages() * PAGE_SIZE;
    kinfo.total_syscalls = __atomic_load_n(&g_total_syscall_count, __ATOMIC_RELAXED);
    if (copy_to_user(u_info, &kinfo, sizeof(kinfo)) != 0) return -STRATUM_EFAULT;
    return 0;
}

static int64_t sys_pair_impl(int *out, uint64_t flags, bool channel) {
    if (flags & ~(uint64_t)(O_CLOEXEC | O_NONBLOCK)) return -STRATUM_EINVAL;
    if (!is_user_range_valid(out, 2 * sizeof(int))) return -STRATUM_EFAULT;

    process_t *proc = process_get_current();
    if (!proc) return -STRATUM_ESRCH;

    file_t *first, *second;
    int res = channel ? channel_create(&first, &second) : pipe_create(&first, &second);
    if (res) return res;
    first->flags = second->flags = flags & O_NONBLOCK;

    int a = fd_alloc_flags(&proc->fds, first, flags & O_CLOEXEC);
    if (a < 0) {
        file_close(first);
        file_close(second);
        return a;
    }
    int b = fd_alloc_flags(&proc->fds, second, flags & O_CLOEXEC);
    if (b < 0) {
        fd_close(&proc->fds, a);
        file_close(second);
        return b;
    }

    int descriptors[2] = { a, b };
    if (copy_to_user(out, descriptors, sizeof(descriptors))) {
        fd_close(&proc->fds, a);
        fd_close(&proc->fds, b);
        return -STRATUM_EFAULT;
    }
    return 0;
}

static int64_t sys_poll_impl(struct pollfd *u_fds, uint32_t nfds, int timeout_ms) {
    if (!is_user_range_valid(u_fds, nfds * sizeof(struct pollfd))) {
        return -STRATUM_EFAULT;
    }

    process_t *proc = process_get_current();
    if (!proc) return -STRATUM_ESRCH;

    struct pollfd k_fds[32];
    if (copy_from_user(k_fds, u_fds, nfds * sizeof(struct pollfd)) != 0) {
        return -STRATUM_EFAULT;
    }

    uint64_t identities[32];
    for (uint32_t i = 0; i < nfds; i++) identities[i] = fd_generation(&proc->fds, k_fds[i].fd);

    uint64_t deadline = timeout_ms < 0 ? UINT64_MAX : event_deadline((uint64_t)timeout_ms);
    for (;;) {
        uint64_t generation = readiness_generation();
        int ready_count = 0;
        bool legacy_console = false;

        for (uint32_t i = 0; i < nfds; i++) {
            struct pollfd *pollfd = &k_fds[i];
            pollfd->revents = 0;
            if (pollfd->fd < 0) continue;

            file_t *file = fd_get_generation(&proc->fds, pollfd->fd, identities[i]);
            if (!file) {
                pollfd->revents = POLLNVAL;
            } else {
                if (file->ops && file->ops->poll) pollfd->revents = (int16_t)file->ops->poll(file, (uint32_t)pollfd->events);
                if (file->type == FILE_TYPE_CONSOLE && (pollfd->events & POLLIN) && !uart_rx_uses_irq()) legacy_console = true;
                file_close(file);
            }
            if (pollfd->revents) ready_count++;
        }

        uint64_t now = timer_get_ticks();
        if (ready_count || !timeout_ms || now >= deadline) {
            int result = copy_to_user(u_fds, k_fds, nfds * sizeof(struct pollfd));
            return result ? result : ready_count;
        }

        /* UART fallback is needed on platforms without a routable I/O APIC. */
        uint64_t wake_at = legacy_console && now + 1 < deadline ? now + 1 : deadline;
        if (readiness_wait(generation, wake_at) == -STRATUM_EINTR) return -STRATUM_EINTR;
    }
}

static int64_t dispatch_syscall(syscall_regs_t *regs) {
    if (!regs) return -STRATUM_EINVAL;

    __atomic_add_fetch(&g_total_syscall_count, 1, __ATOMIC_RELAXED);
    uint64_t nr = regs->rax;

    if (nr != SYS_trace_info && nr != SYS_trace_read && nr != SYS_trace_emit) {
        trace_emit(TRACE_EVENT_SYSCALL, nr, regs->rdi, regs->rsi, regs->rdx);
    }

    switch (nr) {
    case SYS_exit: {
        int status = (int)regs->rdi;
        process_exit(status);
        return 0;
    }

    case SYS_fork: {
        return process_fork(regs, -1);
    }

    case SYS_fork_on_cpu:
        if (regs->rdi >= smp_get_online_cpus()) return -STRATUM_EINVAL;
        return process_fork(regs, (int)regs->rdi);

    case SYS_execve:
        return process_exec((const char *)regs->rdi, (const char *const *)regs->rsi,
                            (const char *const *)regs->rdx);

    case SYS_waitpid: {
        int32_t pid = (int32_t)regs->rdi;
        int *status = (int *)regs->rsi;
        int options = (int)regs->rdx;
        return process_waitpid(pid, status, options);
    }

    case SYS_thread_create:
        return process_thread_create(regs->rdi, regs->rsi, regs->rdx, regs->r10);

    case SYS_thread_exit:
        process_exit_thread((int)regs->rdi);

    case SYS_thread_cancel:
        return process_thread_cancel(regs->rdi);

    case SYS_thread_join:
        return process_thread_join(regs->rdi, (int *)regs->rsi);

    case SYS_thread_self:
        return process_thread_self();

    case SYS_futex:
        return futex_call(regs->rdi, regs->rsi, regs->rdx, regs->r10);

    case SYS_arch_prctl:
        return process_arch_prctl(regs->rdi, regs->rsi);

    case SYS_sched_yield:
        thread_yield();
        return 0;

    case SYS_getcpu:
        return smp_get_cpu_id();

    case SYS_blk_irq_wakes:
        return (int64_t)virtio_blk_irq_wakes();

    case SYS_fs_cache: {
        strat_cache_stats_t stats;
        int result = stratafs_cache_snapshot(&stats);
        return result ? result : copy_to_user((void *)regs->rdi, &stats, sizeof(stats));
    }

    case SYS_resources: {
        strat_resources_t resources = {
            .threads = sched_live_threads(),
            .file_objects = file_live_objects(),
            .packet_buffers = pbuf_live_count(),
        };
        stratafs_journal_usage(&resources.journal_capacity, &resources.journal_pending);
        return copy_to_user((void *)regs->rdi, &resources, sizeof(resources));
    }

    case SYS_process_info: {
        strat_process_info_t info = {0};
        int result = process_info(regs->rdi, &info);
        if (result <= 0) return result;
        int copied = copy_to_user((void *)regs->rsi, &info, sizeof(info));
        return copied ? copied : 1;
    }

    case SYS_tcp_recovery:
        return (int64_t)tcp_fast_retransmits();

    case SYS_net_rx_irqs:
        return (int64_t)virtio_net_rx_irq_count();

    case SYS_net_queues: {
        virtio_net_dev_t *net = virtio_net_get_primary();
        if (!net) return -STRATUM_ENODEV;
        strat_queue_stats_t queues[2];
        virtqueue_snapshot(net->rx_vq, &queues[0]);
        virtqueue_snapshot(net->tx_vq, &queues[1]);
        return copy_to_user((void *)regs->rdi, queues, sizeof(queues));
    }

    case SYS_sched_switches:
        if (regs->rdi >= smp_get_online_cpus()) return -STRATUM_EINVAL;
        return (int64_t)__atomic_load_n(&g_runqueues[regs->rdi].user_switches, __ATOMIC_RELAXED);

    case SYS_mmap: {
        bool shared = regs->r10 == (MAP_SHARED | MAP_ANONYMOUS);
        if (regs->rdi || (!shared && regs->r10 != (MAP_PRIVATE | MAP_ANONYMOUS)) ||
            regs->r8 != (uint64_t)-1 || regs->r9 || regs->rdx > (PROT_READ | PROT_WRITE)) {
            return -STRATUM_EINVAL;
        }
        return user_map_anonymous(process_get_current(), regs->rsi, (unsigned)regs->rdx, shared);
    }

    case SYS_munmap:
        return user_unmap(process_get_current(), regs->rdi, regs->rsi);

    case SYS_mprotect:
        return user_protect(process_get_current(), regs->rdi, regs->rsi, (unsigned)regs->rdx);

    case SYS_getpid: {
        process_t *p = process_get_current();
        return p ? (int64_t)p->pid : 1;
    }

    case SYS_read: {
        return sys_read_impl((int)regs->rdi, (void *)regs->rsi, (size_t)regs->rdx);
    }

    case SYS_write: {
        return sys_write_impl((int)regs->rdi, (const void *)regs->rsi, (size_t)regs->rdx);
    }

    case SYS_open: {
        return sys_open_impl((const char *)regs->rdi, (int)regs->rsi, (int)regs->rdx);
    }

    case SYS_close: {
        process_t *p = process_get_current();
        if (!p) return -STRATUM_ESRCH;
        return fd_close(&p->fds, (int)regs->rdi);
    }

    case SYS_lseek: {
        return sys_lseek_impl((int)regs->rdi, (int64_t)regs->rsi, (int)regs->rdx);
    }

    case SYS_stat: {
        return sys_stat_impl((const char *)regs->rdi, (vfs_stat_t *)regs->rsi);
    }

    case SYS_mkdir: {
        return sys_mkdir_impl((const char *)regs->rdi, (int)regs->rsi);
    }

    case SYS_unlink:
    case SYS_rmdir:
        return sys_remove_impl((const char *)regs->rdi, nr == SYS_rmdir);

    case SYS_rename:
    case SYS_link:
    case SYS_symlink:
        return sys_move_impl((const char *)regs->rdi, (const char *)regs->rsi, nr);

    case SYS_readlink:
        return sys_readlink_impl((const char *)regs->rdi, (char *)regs->rsi, regs->rdx);

    case SYS_readdir:
        return sys_readdir_impl((int)regs->rdi, (strat_dirent_t *)regs->rsi);

    case SYS_fsync: {
        return sys_fsync_impl((int)regs->rdi);
    }

    case SYS_dup2: {
        process_t *p = process_get_current();
        if (!p) return -STRATUM_ESRCH;
        return fd_dup2(&p->fds, (int)regs->rdi, (int)regs->rsi);
    }

    case SYS_pipe: {
        return sys_pair_impl((int *)regs->rdi, 0, false);

    case SYS_pipe2:
        return sys_pair_impl((int *)regs->rdi, regs->rsi, false);

    case SYS_channel_pair:
        return sys_pair_impl((int *)regs->rdi, regs->rsi, true);
    }

    case SYS_poll: {
        if (regs->rsi > 32) return -STRATUM_EINVAL;
        return sys_poll_impl((struct pollfd *)regs->rdi, (uint32_t)regs->rsi, (int)regs->rdx);
    }

    case SYS_socket: {
        return sys_socket((int)regs->rdi, (int)regs->rsi, (int)regs->rdx);
    }

    case SYS_bind: {
        return sys_bind_user((int)regs->rdi, (const struct sockaddr *)regs->rsi, (uint32_t)regs->rdx);
    }

    case SYS_listen: {
        return sys_listen((int)regs->rdi, (int)regs->rsi);
    }

    case SYS_accept: {
        return sys_accept_user((int)regs->rdi, (struct sockaddr *)regs->rsi, (uint32_t *)regs->rdx);
    }

    case SYS_connect: {
        return sys_connect_user((int)regs->rdi, (const struct sockaddr *)regs->rsi, (uint32_t)regs->rdx);
    }

    case SYS_send: {
        return sys_send_user((int)regs->rdi, (const void *)regs->rsi, (size_t)regs->rdx, (int)regs->r10);
    }

    case SYS_recv: {
        return sys_recv_user((int)regs->rdi, (void *)regs->rsi, (size_t)regs->rdx, (int)regs->r10);
    }

    case SYS_shutdown: {
        return sys_shutdown((int)regs->rdi, (int)regs->rsi);
    }

    case SYS_nanosleep: {
        uint32_t ms = (uint32_t)regs->rdi;
        thread_sleep_ms(ms);
        return 0;
    }

    case SYS_clock_gettime: {
        return (int64_t)timer_get_uptime_ms();
    }

    case SYS_trace_info: {
        if (regs->rdi > UINT32_MAX) return -STRATUM_EINVAL;
        strat_trace_info_t info;
        int result = trace_info((uint32_t)regs->rdi, &info);
        return result ? result : copy_to_user((void *)regs->rsi, &info, sizeof(info));
    }

    case SYS_trace_read: {
        if (regs->rdi > UINT32_MAX) return -STRATUM_EINVAL;
        strat_trace_record_t record;
        int result = trace_read((uint32_t)regs->rdi, regs->rsi, &record);
        return result ? result : copy_to_user((void *)regs->rdx, &record, sizeof(record));
    }

    case SYS_trace_emit: {
        if (regs->rdi < STRAT_TRACE_USER_BASE || regs->rdi > UINT32_MAX) return -STRATUM_EINVAL;
        trace_emit((uint32_t)regs->rdi, regs->rsi, regs->rdx, regs->r10, regs->r8);
        return 0;
    }

    case SYS_sysinfo: {
        return sys_sysinfo_impl((strat_sysinfo_t *)regs->rdi);
    }

    default:
        return -STRATUM_ENOSYS;
    }
}

int64_t syscall_dispatch(syscall_regs_t *regs) {
    if (thread_cancelled()) process_exit_thread(-STRATUM_EINTR);
    int64_t result = dispatch_syscall(regs);
    if (thread_cancelled()) process_exit_thread(-STRATUM_EINTR);
    return result;
}
