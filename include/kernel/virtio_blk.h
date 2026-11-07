#ifndef STRATUM_KERNEL_VIRTIO_BLK_H
#define STRATUM_KERNEL_VIRTIO_BLK_H

#include <kernel/types.h>
#include <kernel/virtio.h>
#include <kernel/mutex.h>

#define VIRTIO_BLK_PCI_LEGACY_DEVICE_ID 0x1001
#define VIRTIO_BLK_PCI_MODERN_DEVICE_ID 0x1042

#define VIRTIO_BLK_F_RO       (1ULL << 5)
#define VIRTIO_BLK_F_BLK_SIZE (1ULL << 6)
#define VIRTIO_BLK_F_FLUSH    (1ULL << 9)

#define VIRTIO_BLK_T_IN       0
#define VIRTIO_BLK_T_OUT      1
#define VIRTIO_BLK_T_FLUSH    4

#define VIRTIO_BLK_S_OK       0
#define VIRTIO_BLK_S_IOERR    1
#define VIRTIO_BLK_S_UNSUPP   2

#define VIRTIO_BLK_SECTOR_SIZE 512

typedef struct virtio_blk_config {
    uint64_t capacity;     /* Sectors (512 bytes each) */
    uint32_t size_max;
    uint32_t seg_max;
    struct {
        uint16_t cylinders;
        uint8_t  heads;
        uint8_t  sectors;
    } geometry;
    uint32_t blk_size;
} __attribute__((packed)) virtio_blk_config_t;

typedef struct virtio_blk_req_hdr {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
} __attribute__((packed)) virtio_blk_req_hdr_t;

typedef struct virtio_blk_dev {
    virtio_device_t    vdev;
    virtqueue_t       *vq;
    uint64_t           capacity_sectors;
    uint32_t           block_size;
    bool               read_only;
    bool               failed;
    mutex_t            lock;
    uint8_t            irq_apic;
    bool               irq_enabled;
} virtio_blk_dev_t;

int               virtio_blk_init(void);
virtio_blk_dev_t *virtio_blk_get_primary(void);
int64_t           virtio_blk_read(virtio_blk_dev_t *dev, uint64_t sector, uint32_t count, void *buf);
int64_t           virtio_blk_write(virtio_blk_dev_t *dev, uint64_t sector, uint32_t count, const void *buf);
uint64_t          virtio_blk_irq_wakes(void);
int               virtio_blk_reset(virtio_blk_dev_t *dev);
int               virtio_blk_flush(virtio_blk_dev_t *dev);

#endif /* STRATUM_KERNEL_VIRTIO_BLK_H */
