/* Driver campaign on a disposable disk seeded by the host harness. */
#include <kernel/kernel.h>
#include <kernel/virtio_blk.h>
#include <kernel/x86_64.h>

#define READS 150000U

void block_wrap_test(void) {
    virtio_blk_dev_t *disk = virtio_blk_get_primary();
    kassert(disk != NULL && disk->capacity_sectors >= 8);
    virtqueue_t *queue = disk->vq;
    uint16_t available = queue->avail->idx;
    uint16_t used = queue->last_used_idx;
    uint32_t available_wraps = 0, used_wraps = 0;
    static uint8_t data[4096];

    for (unsigned i = 0; i < READS; i++) {
        uint32_t sectors = 1U << (i % 4);
        uint32_t bytes = sectors * VIRTIO_BLK_SECTOR_SIZE;
        kassert(virtio_blk_read(disk, 0, sectors, data) == bytes);
        for (uint32_t j = 0; j < bytes; j++) {
            kassert(data[j] == (uint8_t)(j * 17 + (j >> 8)));
        }

        uint16_t next_available = queue->avail->idx;
        uint16_t next_used = queue->last_used_idx;
        kassert(next_available == (uint16_t)(available + 1));
        kassert(next_used == (uint16_t)(used + 1));
        if (next_available == 0) available_wraps++;
        if (next_used == 0) used_wraps++;
        available = next_available;
        used = next_used;
    }

    kassert(available_wraps == 2 && used_wraps == 2);
    kassert(queue->num_free == queue->queue_size);
    kprintf("[TEST] Block campaign complete: %u reads, %u available wraps, %u used wraps.\n",
            READS, available_wraps, used_wraps);
    cli();
    for (;;) hlt();
}
