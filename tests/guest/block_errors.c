#include <kernel/kernel.h>
#include <kernel/pmm.h>
#include <kernel/virtio_blk.h>
#include <kernel/x86_64.h>
#include <shared/errno.h>

#define SATURATIONS 500U
#define RESETS 500U
#define ERRORS 1000U
#define MAX_REQUESTS 128U
#define ERROR_SECTOR 64U

typedef struct {
    virtio_blk_req_hdr_t header;
    uint8_t status;
} pressure_request_t;

static pressure_request_t requests[MAX_REQUESTS];
static _Alignas(PAGE_SIZE) uint8_t payloads[MAX_REQUESTS][VIRTIO_BLK_SECTOR_SIZE];

static int enqueue(virtqueue_t *queue, unsigned slot) {
    pressure_request_t *request = &requests[slot];
    request->header = (virtio_blk_req_hdr_t){ .type = VIRTIO_BLK_T_IN };
    request->status = 0xff;
    vring_buf_t buffers[] = {
        { .phys_addr = virt_to_phys(&request->header), .len = sizeof(request->header) },
        { .phys_addr = virt_to_phys(payloads[slot]), .len = sizeof(payloads[slot]), .is_write = true },
        { .phys_addr = virt_to_phys(&request->status), .len = 1, .is_write = true },
    };
    return virtqueue_add_buf(queue, buffers, 1, 2, request);
}

static void verify_sector(const uint8_t *data) {
    for (unsigned i = 0; i < VIRTIO_BLK_SECTOR_SIZE; i++) {
        kassert(data[i] == (uint8_t)(i * 17 + (i >> 8)));
    }
}

static void saturate(virtqueue_t *queue) {
    unsigned count = queue->queue_size / 3;
    kassert(count && count < MAX_REQUESTS);
    for (unsigned i = 0; i < count; i++) kassert(enqueue(queue, i) == 0);
    kassert(enqueue(queue, count) == -STRATUM_ENOMEM);
    kassert(queue->num_free == queue->queue_size % 3);
    virtqueue_kick(queue);

    bool seen[MAX_REQUESTS] = {0};
    for (unsigned i = 0; i < count; i++) {
        uint32_t length = 0;
        void *done = NULL;
        for (unsigned tries = 0; tries < 10000000 && !done; tries++) {
            done = virtqueue_get_buf(queue, &length);
            if (!done) cpu_pause();
        }
        kassert(done && length == VIRTIO_BLK_SECTOR_SIZE + 1);
        unsigned slot = (unsigned)((pressure_request_t *)done - requests);
        kassert(slot < count && !seen[slot] && requests[slot].status == VIRTIO_BLK_S_OK);
        seen[slot] = true;
        verify_sector(payloads[slot]);
    }
    kassert(queue->num_free == queue->queue_size && queue->submitted == queue->completed);
}

void block_error_test(void) {
    virtio_blk_dev_t *disk = virtio_blk_get_primary();
    kassert(disk && disk->capacity_sectors > ERROR_SECTOR);
    uint8_t sector[VIRTIO_BLK_SECTOR_SIZE];
    for (unsigned i = 0; i < SATURATIONS; i++) saturate(disk->vq);
    kprintf("[TEST] Block saturation: %u cases with completed device reads and full reclamation.\n", SATURATIONS);

    /* Publish an unconsumed request, then verify a fresh ring works after reset. */
    kassert(virtio_blk_reset(disk) == 0);
    uint64_t baseline = pmm_get_free_pages();
    for (unsigned i = 0; i < RESETS; i++) {
        kassert(enqueue(disk->vq, 0) == 0);
        kassert(virtio_blk_reset(disk) == 0);
        kassert(disk->vq->num_free == disk->vq->queue_size && !disk->vq->submitted && !disk->vq->completed);
        kassert(virtio_blk_read(disk, 0, 1, sector) == sizeof(sector));
        verify_sector(sector);
    }
    kassert(pmm_get_free_pages() == baseline);
    kprintf("[TEST] Block reset: %u cases with old queues retired and restored device capacity.\n", RESETS);

    for (unsigned i = 0; i < ERRORS; i++) {
        uint64_t completed = disk->vq->completed;
        kassert(virtio_blk_read(disk, ERROR_SECTOR, 1, sector) == -STRATUM_EIO);
        kassert(disk->vq->completed == completed + 1 && disk->vq->num_free == disk->vq->queue_size);
        kassert(virtio_blk_read(disk, 0, 1, sector) == sizeof(sector));
        verify_sector(sector);
    }
    kprintf("[TEST] Block I/O errors: %u device failures with continued reads and no descriptor leaks.\n", ERRORS);
    cli();
    for (;;) hlt();
}
