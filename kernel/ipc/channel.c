#include <kernel/channel.h>
#include <kernel/event.h>
#include <kernel/sched.h>
#include <kernel/slab.h>
#include <kernel/string.h>
#include <kernel/syscall.h>
#include <shared/errno.h>
#include <shared/syscall_nums.h>

typedef struct channel_pair channel_pair_t;

typedef struct {
    channel_pair_t *pair;
    unsigned index;
} channel_endpoint_t;

typedef struct {
    uint16_t size;
    uint8_t bytes[STRAT_CHANNEL_MAX_MESSAGE];
} channel_message_t;

typedef struct {
    unsigned head, count;
    channel_message_t messages[STRAT_CHANNEL_QUEUE_CAPACITY];
    wait_queue_t readable, writable;
} channel_queue_t;

struct channel_pair {
    spinlock_t lock;
    unsigned open;
    channel_endpoint_t endpoints[2];
    channel_queue_t incoming[2];
};

int64_t channel_read_user(file_t *file, void *buffer, size_t count) {
    if (!count) return 0;
    channel_endpoint_t *end = file->priv;
    channel_pair_t *pair = end->pair;
    channel_queue_t *queue = &pair->incoming[end->index];
    unsigned peer = end->index ^ 1U;
    for (;;) {
        uint64_t flags;
        spin_lock_irqsave(&pair->lock, &flags);
        if (queue->count) {
            channel_message_t *msg = &queue->messages[queue->head];
            int size = msg->size;
            int result = count < (size_t)size ? -STRATUM_EMSGSIZE : copy_to_user(buffer, msg->bytes, (size_t)size);
            if (!result) {
                queue->head = (queue->head + 1) % STRAT_CHANNEL_QUEUE_CAPACITY;
                if (queue->count-- == STRAT_CHANNEL_QUEUE_CAPACITY) readiness_notify();
                wait_queue_wake_one(&queue->writable);
            }
            spin_unlock_irqrestore(&pair->lock, flags);
            return result ? result : size;
        }
        if (!(pair->open & (1U << peer))) {
            spin_unlock_irqrestore(&pair->lock, flags);
            return 0;
        }
        if (file->flags & O_NONBLOCK) {
            spin_unlock_irqrestore(&pair->lock, flags);
            return -STRATUM_EAGAIN;
        }
        int interrupted = wait_queue_wait_locked(&queue->readable, &pair->lock, flags);
        if (interrupted) return interrupted;
    }
}

static int64_t channel_write(file_t *file, const void *buffer, size_t count) {
    if (!count) return 0;
    if (count > STRAT_CHANNEL_MAX_MESSAGE) return -STRATUM_EMSGSIZE;
    channel_endpoint_t *end = file->priv;
    channel_pair_t *pair = end->pair;
    unsigned peer = end->index ^ 1U;
    channel_queue_t *queue = &pair->incoming[peer];
    for (;;) {
        uint64_t flags;
        spin_lock_irqsave(&pair->lock, &flags);
        if (!(pair->open & (1U << peer))) {
            spin_unlock_irqrestore(&pair->lock, flags);
            return -STRATUM_EPIPE;
        }
        if (queue->count < STRAT_CHANNEL_QUEUE_CAPACITY) {
            unsigned tail = (queue->head + queue->count) % STRAT_CHANNEL_QUEUE_CAPACITY;
            channel_message_t *msg = &queue->messages[tail];
            msg->size = (uint16_t)count;
            memcpy(msg->bytes, buffer, count);
            if (queue->count++ == 0) readiness_notify();
            wait_queue_wake_all(&queue->readable);
            spin_unlock_irqrestore(&pair->lock, flags);
            return (int64_t)count;
        }
        if (file->flags & O_NONBLOCK) {
            spin_unlock_irqrestore(&pair->lock, flags);
            return -STRATUM_EAGAIN;
        }
        int interrupted = wait_queue_wait_locked(&queue->writable, &pair->lock, flags);
        if (interrupted) return interrupted;
    }
}

static int channel_poll(file_t *file, uint32_t events) {
    channel_endpoint_t *end = file->priv;
    channel_pair_t *pair = end->pair;
    unsigned peer = end->index ^ 1U;
    uint64_t flags;
    spin_lock_irqsave(&pair->lock, &flags);
    bool connected = pair->open & (1U << peer);
    int ready = connected ? 0 : POLLHUP;
    if (pair->incoming[end->index].count || !connected) ready |= events & POLLIN;
    if (connected && pair->incoming[peer].count < STRAT_CHANNEL_QUEUE_CAPACITY) ready |= events & POLLOUT;
    spin_unlock_irqrestore(&pair->lock, flags);
    return ready;
}

static int channel_close(file_t *file) {
    channel_endpoint_t *end = file->priv;
    channel_pair_t *pair = end->pair;
    uint64_t flags;
    spin_lock_irqsave(&pair->lock, &flags);
    pair->open &= ~(1U << end->index);
    pair->incoming[end->index].count = 0;
    wait_queue_wake_all(&pair->incoming[end->index].writable);
    wait_queue_wake_all(&pair->incoming[end->index ^ 1U].readable);
    readiness_notify();
    bool unused = !pair->open;
    spin_unlock_irqrestore(&pair->lock, flags);
    if (unused) kfree(pair);
    return 0;
}

static const file_ops_t channel_ops = {
    .write = channel_write,
    .poll = channel_poll,
    .close = channel_close,
};

int channel_create(file_t **first, file_t **second) {
    channel_pair_t *pair = kzalloc(sizeof(*pair));
    if (!pair) return -STRATUM_ENOMEM;
    spin_lock_init(&pair->lock);
    for (unsigned i = 0; i < 2; i++) {
        pair->endpoints[i] = (channel_endpoint_t){ .pair = pair, .index = i };
        wait_queue_init(&pair->incoming[i].readable);
        wait_queue_init(&pair->incoming[i].writable);
    }

    file_t *a = file_alloc(FILE_TYPE_CHANNEL, &channel_ops, &pair->endpoints[0]);
    if (!a) {
        kfree(pair);
        return -STRATUM_ENOMEM;
    }
    pair->open = 1;
    file_t *b = file_alloc(FILE_TYPE_CHANNEL, &channel_ops, &pair->endpoints[1]);
    if (!b) {
        file_close(a);
        return -STRATUM_ENOMEM;
    }
    pair->open = 3;
    *first = a;
    *second = b;
    return 0;
}
