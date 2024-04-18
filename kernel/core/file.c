#include <kernel/file.h>
#include <kernel/slab.h>
#include <kernel/kernel.h>
#include <kernel/string.h>
#include <kernel/uart.h>
#include <kernel/sched.h>
#include <shared/errno.h>
#include <shared/syscall_nums.h>

static kmem_cache_t *g_file_cache = NULL;

static int64_t console_read(file_t *file, void *buf, size_t count) {
    (void)file;
    if (count == 0) return 0;

    int ch;
    while ((ch = uart_getc_nonblocking()) < 0) {
        thread_sleep_ms(10);
    }
    ((char *)buf)[0] = (char)ch;
    return 1;
}

static int64_t console_write(file_t *file, const void *buf, size_t count) {
    (void)file;
    if (!buf || count == 0) return 0;
    uart_write((const char *)buf, count);
    return (int64_t)count;
}

static int console_poll(file_t *file, uint32_t events) {
    (void)file;
    int revents = 0;
    if ((events & POLLIN) && uart_input_ready()) revents |= POLLIN;
    if (events & POLLOUT) {
        revents |= POLLOUT;
    }
    return revents;
}

static int console_close(file_t *file) {
    (void)file;
    return 0;
}

static const file_ops_t g_console_ops = {
    .read  = console_read,
    .write = console_write,
    .poll  = console_poll,
    .close = console_close,
};

void file_init(void) {
    g_file_cache = kmem_cache_create("file_cache", sizeof(file_t), 8);
    kassert(g_file_cache != NULL);
}

file_t *file_alloc(file_type_t type, const file_ops_t *ops, void *priv) {
    if (!g_file_cache) {
        file_init();
    }

    file_t *f = (file_t *)kmem_cache_alloc(g_file_cache);
    if (!f) return NULL;

    memset(f, 0, sizeof(file_t));
    f->type = type;
    f->ops = ops;
    f->priv = priv;
    f->refcount = 1;
    spin_lock_init(&f->lock);

    return f;
}

void file_ref(file_t *f) {
    if (!f) return;
    __atomic_add_fetch(&f->refcount, 1, __ATOMIC_SEQ_CST);
}

void file_close(file_t *f) {
    if (!f) return;

    if (__atomic_sub_fetch(&f->refcount, 1, __ATOMIC_SEQ_CST) == 0) {
        if (f->ops && f->ops->close) {
            f->ops->close(f);
        }
        kmem_cache_free(g_file_cache, f);
    }
}

file_t *file_create_console(void) {
    return file_alloc(FILE_TYPE_CONSOLE, &g_console_ops, NULL);
}

void fd_table_init(fd_table_t *table) {
    if (!table) return;
    spin_lock_init(&table->lock);
    for (int i = 0; i < PROCESS_MAX_FDS; i++) {
        table->files[i] = NULL;
        table->flags[i] = 0;
    }
}

void fd_table_copy(fd_table_t *dst, const fd_table_t *src) {
    if (!dst || !src) return;
    fd_table_init(dst);

    for (int i = 0; i < PROCESS_MAX_FDS; i++) {
        file_t *f = src->files[i];
        if (f) {
            file_ref(f);
            dst->files[i] = f;
            dst->flags[i] = src->flags[i];
        }
    }
}

void fd_table_destroy(fd_table_t *table) {
    if (!table) return;
    for (int i = 0; i < PROCESS_MAX_FDS; i++) {
        if (table->files[i]) {
            file_close(table->files[i]);
            table->files[i] = NULL;
        }
    }
}

int fd_alloc(fd_table_t *table, file_t *file) {
    return fd_alloc_flags(table, file, 0);
}

void fd_close_exec(fd_table_t *table) {
    file_t *closing[PROCESS_MAX_FDS];
    size_t count = 0;
    uint64_t irq;
    spin_lock_irqsave(&table->lock, &irq);
    for (int i = 0; i < PROCESS_MAX_FDS; i++) {
        if (table->files[i] && (table->flags[i] & O_CLOEXEC)) {
            closing[count++] = table->files[i];
            table->files[i] = NULL;
            table->flags[i] = 0;
        }
    }
    spin_unlock_irqrestore(&table->lock, irq);
    for (size_t i = 0; i < count; i++) file_close(closing[i]);
}

int fd_alloc_flags(fd_table_t *table, file_t *file, uint32_t descriptor_flags) {
    if (!table || !file) return -STRATUM_EINVAL;

    uint64_t flags;
    spin_lock_irqsave(&table->lock, &flags);

    for (int i = 0; i < PROCESS_MAX_FDS; i++) {
        if (!table->files[i]) {
            table->files[i] = file;
            table->flags[i] = descriptor_flags;
            spin_unlock_irqrestore(&table->lock, flags);
            return i;
        }
    }

    spin_unlock_irqrestore(&table->lock, flags);
    return -STRATUM_EMFILE;
}

int fd_install(fd_table_t *table, int fd, file_t *file) {
    if (!table || fd < 0 || fd >= PROCESS_MAX_FDS) return -STRATUM_EBADF;

    uint64_t flags;
    spin_lock_irqsave(&table->lock, &flags);

    file_t *old = table->files[fd];
    table->files[fd] = file;
    table->flags[fd] = 0;

    spin_unlock_irqrestore(&table->lock, flags);

    if (old) {
        file_close(old);
    }
    return 0;
}

file_t *fd_get(fd_table_t *table, int fd) {
    if (!table || fd < 0 || fd >= PROCESS_MAX_FDS) return NULL;

    uint64_t flags;
    spin_lock_irqsave(&table->lock, &flags);
    file_t *f = table->files[fd];
    if (f) {
        file_ref(f);
    }
    spin_unlock_irqrestore(&table->lock, flags);
    return f;
}

int fd_close(fd_table_t *table, int fd) {
    if (!table || fd < 0 || fd >= PROCESS_MAX_FDS) return -STRATUM_EBADF;

    uint64_t flags;
    spin_lock_irqsave(&table->lock, &flags);
    file_t *f = table->files[fd];
    table->files[fd] = NULL;
    table->flags[fd] = 0;
    spin_unlock_irqrestore(&table->lock, flags);

    if (!f) return -STRATUM_EBADF;
    file_close(f);
    return 0;
}

int fd_dup2(fd_table_t *table, int oldfd, int newfd) {
    if (!table || oldfd < 0 || oldfd >= PROCESS_MAX_FDS ||
        newfd < 0 || newfd >= PROCESS_MAX_FDS) {
        return -STRATUM_EBADF;
    }

    uint64_t flags;
    spin_lock_irqsave(&table->lock, &flags);

    file_t *src = table->files[oldfd];
    if (!src) {
        spin_unlock_irqrestore(&table->lock, flags);
        return -STRATUM_EBADF;
    }

    if (oldfd == newfd) {
        spin_unlock_irqrestore(&table->lock, flags);
        return newfd;
    }

    file_ref(src);
    file_t *old_dst = table->files[newfd];
    table->files[newfd] = src;
    table->flags[newfd] = 0;

    spin_unlock_irqrestore(&table->lock, flags);

    if (old_dst) {
        file_close(old_dst);
    }

    return newfd;
}
