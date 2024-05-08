#ifndef STRATUM_KERNEL_VIRTQUEUE_H
#define STRATUM_KERNEL_VIRTQUEUE_H

#include <kernel/types.h>
#include <kernel/spinlock.h>
#include <shared/syscall_nums.h>

#define VRING_DESC_F_NEXT       1
#define VRING_DESC_F_WRITE      2
#define VRING_DESC_F_INDIRECT   4

#define VRING_AVAIL_F_NO_INTERRUPT 1
#define VRING_USED_F_NO_NOTIFY     1

typedef struct vring_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed)) vring_desc_t;

typedef struct vring_avail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[];
} __attribute__((packed)) vring_avail_t;

typedef struct vring_used_elem {
    uint32_t id;
    uint32_t len;
} __attribute__((packed)) vring_used_elem_t;

typedef struct vring_used {
    uint16_t          flags;
    uint16_t          idx;
    vring_used_elem_t ring[];
} __attribute__((packed)) vring_used_t;

typedef struct vring_buf {
    uint64_t phys_addr;
    uint32_t len;
    bool     is_write;  /* true = device writes to buffer (device-writable) */
} vring_buf_t;

typedef void (*virtqueue_notify_fn)(void *ctx, uint16_t queue_idx);

typedef struct virtqueue {
    spinlock_t          lock;
    uint16_t            queue_idx;
    uint16_t            queue_size;
    uint16_t            num_free;
    uint16_t            free_head;
    uint16_t            last_used_idx;
    uint64_t            submitted, completed;
    bool                failed;

    uint64_t            mem_phys;
    size_t              mem_size;
    void               *mem_virt;

    vring_desc_t       *desc;
    vring_avail_t      *avail;
    volatile vring_used_t *used;

    void              **cookies;

    virtqueue_notify_fn notify;
    void               *notify_ctx;
} virtqueue_t;

virtqueue_t *virtqueue_create(uint16_t queue_idx, uint16_t queue_size,
                              virtqueue_notify_fn notify, void *notify_ctx);
void         virtqueue_destroy(virtqueue_t *vq);

int          virtqueue_add_buf(virtqueue_t *vq, const vring_buf_t *bufs,
                               uint32_t num_out, uint32_t num_in, void *cookie);
void         virtqueue_kick(virtqueue_t *vq);
void        *virtqueue_get_buf(virtqueue_t *vq, uint32_t *out_len);
bool         virtqueue_has_used(virtqueue_t *vq);
bool         virtqueue_faulted(virtqueue_t *vq);
void         virtqueue_snapshot(virtqueue_t *vq, strat_queue_stats_t *stats);

#endif /* STRATUM_KERNEL_VIRTQUEUE_H */
