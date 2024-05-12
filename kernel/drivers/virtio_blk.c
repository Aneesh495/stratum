#include <kernel/virtio_blk.h>
#include <kernel/pmm.h>
#include <kernel/slab.h>
#include <kernel/kernel.h>
#include <kernel/string.h>
#include <kernel/x86_64.h>
#include <shared/errno.h>

static virtio_blk_dev_t g_primary_blk;
static bool             g_primary_blk_registered = false;

static int configure_block(virtio_blk_dev_t *dev);
static int reset_locked(virtio_blk_dev_t *dev);

virtio_blk_dev_t *virtio_blk_get_primary(void) {
    return g_primary_blk_registered ? &g_primary_blk : NULL;
}

/* One request is in flight under the device lock. Keep DMA storage alive even
 * if the device times out and completes late. */
static virtio_blk_req_hdr_t request_header;
static uint8_t request_status;
static _Alignas(PAGE_SIZE) uint8_t request_data[PAGE_SIZE];

static int64_t submit_request(virtio_blk_dev_t *dev, uint32_t type, uint64_t sector,
                              uint32_t bytes, void *buf) {
    request_header = (virtio_blk_req_hdr_t){ .type = type, .sector = sector };
    request_status = 0xFF;
    if (bytes && type == VIRTIO_BLK_T_OUT) memcpy(request_data, buf, bytes);

    vring_buf_t buffers[3] = {
        { .phys_addr = virt_to_phys(&request_header), .len = sizeof(request_header) },
    };
    unsigned out = 1, in = 0;
    if (bytes) {
        buffers[1] = (vring_buf_t){
            .phys_addr = virt_to_phys(request_data), .len = bytes, .is_write = type == VIRTIO_BLK_T_IN,
        };
        if (type == VIRTIO_BLK_T_IN) in++;
        else out++;
    }
    buffers[out + in] = (vring_buf_t){
        .phys_addr = virt_to_phys(&request_status), .len = 1, .is_write = true,
    };
    in++;
    int res = virtqueue_add_buf(dev->vq, buffers, out, in, &request_header);
    if (res != 0) return res;
    virtqueue_kick(dev->vq);

    for (uint64_t attempts = 0; attempts < 10000000ULL; attempts++) {
        uint32_t used;
        void *complete = virtqueue_get_buf(dev->vq, &used);
        if (complete) {
            if (complete != &request_header || request_status != VIRTIO_BLK_S_OK ||
                used != (type == VIRTIO_BLK_T_IN ? bytes + 1 : 1)) return -STRATUM_EIO;
            if (bytes && type == VIRTIO_BLK_T_IN) memcpy(buf, request_data, bytes);
            return bytes;
        }
        if (virtqueue_faulted(dev->vq)) {
            dev->failed = true;
            return -STRATUM_EIO;
        }
        cpu_pause();
    }
    dev->failed = true;
    kprintf("[VIRTIO-BLK] Request timed out at sector %lu.\n", sector);
    return -STRATUM_ETIMEDOUT;
}

static int64_t virtio_blk_do_request(virtio_blk_dev_t *dev, uint32_t type,
                                     uint64_t sector, uint32_t count, void *buf) {
    if (!dev) return -STRATUM_EINVAL;
    if (type != VIRTIO_BLK_T_FLUSH && !count) return 0;

    uint64_t flags;
    spin_lock_irqsave(&dev->lock, &flags);
    int error = 0;
    if (dev->failed || !dev->vq) error = -STRATUM_EIO;
    else if (type == VIRTIO_BLK_T_FLUSH) {
        if (!(dev->vdev.negotiated_features & VIRTIO_BLK_F_FLUSH)) error = -STRATUM_ENOTSUP;
    } else if (!buf || sector >= dev->capacity_sectors || count > dev->capacity_sectors - sector) {
        error = -STRATUM_EINVAL;
    } else if (type == VIRTIO_BLK_T_OUT && dev->read_only) error = -STRATUM_EROFS;
    if (error) {
        spin_unlock_irqrestore(&dev->lock, flags);
        return error;
    }
    uint64_t completed = 0;
    int64_t result;
    do {
        uint32_t sectors = count > PAGE_SIZE / VIRTIO_BLK_SECTOR_SIZE
            ? PAGE_SIZE / VIRTIO_BLK_SECTOR_SIZE : count;
        result = submit_request(dev, type, sector, sectors * VIRTIO_BLK_SECTOR_SIZE,
                                buf ? (uint8_t *)buf + completed : NULL);
        if (result < 0) break;
        completed += (uint64_t)result;
        sector += sectors;
        count -= sectors;
    } while (count);
    if (dev->failed) {
        /* Never replay an operation whose outcome is unknown. */
        int reset = reset_locked(dev);
        if (reset) kprintf("[VIRTIO-BLK] Recovery failed: %d.\n", reset);
    }
    spin_unlock_irqrestore(&dev->lock, flags);
    return completed ? (int64_t)completed : result;
}

int64_t virtio_blk_read(virtio_blk_dev_t *dev, uint64_t sector, uint32_t count, void *buf) {
    return virtio_blk_do_request(dev, VIRTIO_BLK_T_IN, sector, count, buf);
}

int64_t virtio_blk_write(virtio_blk_dev_t *dev, uint64_t sector, uint32_t count, const void *buf) {
    return virtio_blk_do_request(dev, VIRTIO_BLK_T_OUT, sector, count, (void *)buf);
}

int virtio_blk_flush(virtio_blk_dev_t *dev) {
    if (!dev) return -STRATUM_ENOTSUP;
    int64_t res = virtio_blk_do_request(dev, VIRTIO_BLK_T_FLUSH, 0, 0, NULL);
    return (res >= 0) ? 0 : (int)res;
}

static int configure_block(virtio_blk_dev_t *dev) {
    /* Negotiate supported features */
    uint64_t features = VIRTIO_BLK_F_BLK_SIZE | VIRTIO_BLK_F_FLUSH | VIRTIO_BLK_F_RO;
    int res = virtio_negotiate_features(&dev->vdev, features);
    if (res != 0) return res;

    /* Setup request virtqueue (queue index 0) */
    dev->vq = virtio_setup_queue(&dev->vdev, 0);
    if (!dev->vq) {
        kprintf("[VIRTIO-BLK] Failed to setup request virtqueue 0!\n");
        return -STRATUM_ENOMEM;
    }

    /* Read device capacity and block size */
    if (dev->vdev.is_modern && dev->vdev.device_cfg) {
        volatile virtio_blk_config_t *cfg = (volatile virtio_blk_config_t *)dev->vdev.device_cfg;
        dev->capacity_sectors = cfg->capacity;
        dev->block_size = (dev->vdev.negotiated_features & VIRTIO_BLK_F_BLK_SIZE) ? cfg->blk_size : 512;
    } else {
        uint32_t lo = inl((uint16_t)(dev->vdev.io_base + 0x14));
        uint32_t hi = inl((uint16_t)(dev->vdev.io_base + 0x18));
        dev->capacity_sectors = ((uint64_t)hi << 32) | lo;
        dev->block_size = 512;
    }

    dev->read_only = (dev->vdev.negotiated_features & VIRTIO_BLK_F_RO) != 0;

    /* Mark driver as operational */
    virtio_driver_ok(&dev->vdev);
    return 0;
}

static int reset_locked(virtio_blk_dev_t *dev) {
    dev->failed = true;
    int res = virtio_reset_device(&dev->vdev);
    if (res) return res;
    dev->vq = NULL;
    res = configure_block(dev);
    if (!res) dev->failed = false;
    return res;
}

int virtio_blk_reset(virtio_blk_dev_t *dev) {
    if (!dev) return -STRATUM_EINVAL;
    uint64_t flags;
    spin_lock_irqsave(&dev->lock, &flags);
    int res = reset_locked(dev);
    spin_unlock_irqrestore(&dev->lock, flags);
    return res;
}

int virtio_blk_init(void) {
    pci_device_t *pci_dev = pci_find_device(0x1AF4, VIRTIO_BLK_PCI_MODERN_DEVICE_ID);
    if (!pci_dev) {
        pci_dev = pci_find_device(0x1AF4, VIRTIO_BLK_PCI_LEGACY_DEVICE_ID);
    }

    if (!pci_dev) {
        kprintf("[VIRTIO-BLK] No Virtio-blk PCI device detected.\n");
        return -STRATUM_ENODEV;
    }

    virtio_blk_dev_t *dev = &g_primary_blk;
    memset(dev, 0, sizeof(virtio_blk_dev_t));
    spin_lock_init(&dev->lock);

    int res = virtio_probe_device(pci_dev, &dev->vdev);
    if (res != 0) {
        kprintf("[VIRTIO-BLK] Failed to probe virtio device: %d\n", res);
        return res;
    }

    res = configure_block(dev);
    if (res) return res;
    g_primary_blk_registered = true;

    kprintf("[VIRTIO-BLK] Block device initialized: capacity=%lu sectors (%lu MiB, block_size=%u, ro=%u)\n",
            dev->capacity_sectors,
            (dev->capacity_sectors * VIRTIO_BLK_SECTOR_SIZE) / (1024 * 1024),
            dev->block_size, dev->read_only);

    return 0;
}
