#include <kernel/pipe.h>
#include <kernel/event.h>
#include <kernel/pmm.h>
#include <kernel/slab.h>
#include <kernel/kernel.h>
#include <kernel/string.h>
#include <shared/errno.h>
#include <shared/syscall_nums.h>

static kmem_cache_t *g_pipe_cache = NULL;

static int64_t pipe_read_op(file_t *file, void *buf, size_t count) {
    if (!file || !file->priv || !buf) return -STRATUM_EINVAL;
    if (count == 0) return 0;

    pipe_t *pipe = (pipe_t *)file->priv;
    char *dst = (char *)buf;
    size_t bytes_read = 0;

    while (bytes_read < count) {
        uint64_t flags;
        spin_lock_irqsave(&pipe->lock, &flags);

        if (pipe->count > 0) {
            /* Copy available bytes from circular buffer */
            size_t available = pipe->count;
            size_t to_read = (count - bytes_read < available) ? (count - bytes_read) : available;

            for (size_t i = 0; i < to_read; i++) {
                dst[bytes_read++] = pipe->buffer[pipe->read_idx];
                pipe->read_idx = (pipe->read_idx + 1) % PIPE_CAPACITY;
            }
            pipe->count -= to_read;
            if (available == PIPE_CAPACITY) readiness_notify();

            /* Wake any blocked writers */
            wait_queue_wake_all(&pipe->write_wait);
            spin_unlock_irqrestore(&pipe->lock, flags);
            break;
        }

        /* Pipe is empty */
        if (pipe->writers == 0) {
            /* All writers closed: EOF */
            spin_unlock_irqrestore(&pipe->lock, flags);
            break;
        }

        if (file->flags & O_NONBLOCK) {
            spin_unlock_irqrestore(&pipe->lock, flags);
            if (bytes_read > 0) break;
            return -STRATUM_EAGAIN;
        }

        /* Block on read wait queue */
        int result = wait_queue_wait_locked(&pipe->read_wait, &pipe->lock, flags);
        if (result) return bytes_read ? (int64_t)bytes_read : result;
    }

    return (int64_t)bytes_read;
}

static int64_t pipe_write_op(file_t *file, const void *buf, size_t count) {
    if (!file || !file->priv || !buf) return -STRATUM_EINVAL;
    if (count == 0) return 0;

    pipe_t *pipe = (pipe_t *)file->priv;
    const char *src = (const char *)buf;
    size_t bytes_written = 0;

    while (bytes_written < count) {
        uint64_t flags;
        spin_lock_irqsave(&pipe->lock, &flags);

        /* If no readers remain, signal broken pipe */
        if (pipe->readers == 0) {
            spin_unlock_irqrestore(&pipe->lock, flags);
            return bytes_written ? (int64_t)bytes_written : -STRATUM_EPIPE;
        }

        size_t space = PIPE_CAPACITY - pipe->count;
        if (space && (count > 512 || space >= count)) {
            size_t to_write = (count - bytes_written < space) ? (count - bytes_written) : space;

            for (size_t i = 0; i < to_write; i++) {
                pipe->buffer[pipe->write_idx] = src[bytes_written++];
                pipe->write_idx = (pipe->write_idx + 1) % PIPE_CAPACITY;
            }
            pipe->count += to_write;
            if (space == PIPE_CAPACITY) readiness_notify();

            /* Wake any blocked readers */
            wait_queue_wake_all(&pipe->read_wait);
            spin_unlock_irqrestore(&pipe->lock, flags);
            continue;
        }

        /* Pipe is full */
        if (file->flags & O_NONBLOCK) {
            spin_unlock_irqrestore(&pipe->lock, flags);
            if (bytes_written > 0) break;
            return -STRATUM_EAGAIN;
        }

        /* Block on write wait queue */
        int result = wait_queue_wait_locked(&pipe->write_wait, &pipe->lock, flags);
        if (result) return bytes_written ? (int64_t)bytes_written : result;
    }

    return (int64_t)bytes_written;
}

static int pipe_read_poll_op(file_t *file, uint32_t events) {
    if (!file || !file->priv) return POLLNVAL;
    pipe_t *pipe = (pipe_t *)file->priv;

    uint64_t flags;
    spin_lock_irqsave(&pipe->lock, &flags);

    int revents = 0;
    if (events & POLLIN) {
        if (pipe->count > 0) {
            revents |= POLLIN;
        } else if (pipe->writers == 0) {
            revents |= (POLLIN | POLLHUP);
        }
    }

    if (pipe->writers == 0) {
        revents |= POLLHUP;
    }

    spin_unlock_irqrestore(&pipe->lock, flags);
    return revents;
}

static int pipe_write_poll_op(file_t *file, uint32_t events) {
    if (!file || !file->priv) return POLLNVAL;
    pipe_t *pipe = (pipe_t *)file->priv;

    uint64_t flags;
    spin_lock_irqsave(&pipe->lock, &flags);

    int revents = 0;
    if (events & POLLOUT) {
        if (pipe->count < PIPE_CAPACITY && pipe->readers > 0) {
            revents |= POLLOUT;
        } else if (pipe->readers == 0) {
            revents |= (POLLOUT | POLLERR);
        }
    }

    if (pipe->readers == 0) {
        revents |= POLLERR;
    }

    spin_unlock_irqrestore(&pipe->lock, flags);
    return revents;
}

static int pipe_read_close_op(file_t *file) {
    if (!file || !file->priv) return 0;
    pipe_t *pipe = (pipe_t *)file->priv;

    uint64_t flags;
    spin_lock_irqsave(&pipe->lock, &flags);

    if (pipe->readers > 0) {
        pipe->readers--;
    }
    wait_queue_wake_all(&pipe->write_wait);

    readiness_notify();
    bool should_free = (pipe->readers == 0 && pipe->writers == 0);
    spin_unlock_irqrestore(&pipe->lock, flags);

    if (should_free) {
        if (pipe->buffer) {
            pmm_free_page(virt_to_phys(pipe->buffer));
        }
        kmem_cache_free(g_pipe_cache, pipe);
    }
    return 0;
}

static int pipe_write_close_op(file_t *file) {
    if (!file || !file->priv) return 0;
    pipe_t *pipe = (pipe_t *)file->priv;

    uint64_t flags;
    spin_lock_irqsave(&pipe->lock, &flags);

    if (pipe->writers > 0) {
        pipe->writers--;
    }
    wait_queue_wake_all(&pipe->read_wait);

    readiness_notify();
    bool should_free = (pipe->readers == 0 && pipe->writers == 0);
    spin_unlock_irqrestore(&pipe->lock, flags);

    if (should_free) {
        if (pipe->buffer) {
            pmm_free_page(virt_to_phys(pipe->buffer));
        }
        kmem_cache_free(g_pipe_cache, pipe);
    }
    return 0;
}

static const file_ops_t g_pipe_read_ops = {
    .read  = pipe_read_op,
    .write = NULL,
    .poll  = pipe_read_poll_op,
    .close = pipe_read_close_op,
};

static const file_ops_t g_pipe_write_ops = {
    .read  = NULL,
    .write = pipe_write_op,
    .poll  = pipe_write_poll_op,
    .close = pipe_write_close_op,
};

void pipe_init(void) {
    if (!g_pipe_cache) {
        g_pipe_cache = kmem_cache_create("pipe_cache", sizeof(pipe_t), 16);
        kassert(g_pipe_cache != NULL);
    }
}

int pipe_create(file_t **read_file, file_t **write_file) {
    if (!read_file || !write_file) return -STRATUM_EINVAL;

    if (!g_pipe_cache) {
        pipe_init();
    }

    pipe_t *pipe = (pipe_t *)kmem_cache_alloc(g_pipe_cache);
    if (!pipe) return -STRATUM_ENOMEM;

    uint64_t buf_paddr = pmm_alloc_page();
    if (!buf_paddr) {
        kmem_cache_free(g_pipe_cache, pipe);
        return -STRATUM_ENOMEM;
    }

    memset(pipe, 0, sizeof(pipe_t));
    pipe->buffer = (char *)phys_to_virt(buf_paddr);
    spin_lock_init(&pipe->lock);
    pipe->readers = 1;
    pipe->writers = 1;
    wait_queue_init(&pipe->read_wait);
    wait_queue_init(&pipe->write_wait);

    file_t *rf = file_alloc(FILE_TYPE_PIPE, &g_pipe_read_ops, pipe);
    if (!rf) {
        pmm_free_page(buf_paddr);
        kmem_cache_free(g_pipe_cache, pipe);
        return -STRATUM_ENOMEM;
    }

    file_t *wf = file_alloc(FILE_TYPE_PIPE, &g_pipe_write_ops, pipe);
    if (!wf) {
        pipe->writers = 0;
        file_close(rf);
        return -STRATUM_ENOMEM;
    }

    *read_file = rf;
    *write_file = wf;
    return 0;
}
