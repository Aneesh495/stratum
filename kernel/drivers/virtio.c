#include <kernel/virtio.h>
#include <kernel/x86_64.h>
#include <kernel/kernel.h>
#include <kernel/string.h>
#include <kernel/pmm.h>
#include <shared/errno.h>

static void virtio_notify_modern(void *ctx, uint16_t queue_idx) {
    virtio_device_t *vdev = (virtio_device_t *)ctx;
    *vdev->doorbells[queue_idx] = queue_idx;
}

static void virtio_notify_legacy(void *ctx, uint16_t queue_idx) {
    virtio_device_t *vdev = (virtio_device_t *)ctx;
    if (!vdev) return;
    outw((uint16_t)(vdev->io_base + 0x10), queue_idx);
}

void virtio_set_status(virtio_device_t *vdev, uint8_t status) {
    if (!vdev) return;
    if (vdev->is_modern) {
        vdev->common_cfg->device_status = status;
    } else {
        outb((uint16_t)(vdev->io_base + 0x12), status);
    }
}

uint8_t virtio_get_status(virtio_device_t *vdev) {
    if (!vdev) return 0;
    if (vdev->is_modern) {
        return vdev->common_cfg->device_status;
    } else {
        return inb((uint16_t)(vdev->io_base + 0x12));
    }
}

int virtio_probe_device(pci_device_t *pci_dev, virtio_device_t *vdev) {
    if (!pci_dev || !vdev) return -STRATUM_EINVAL;

    memset(vdev, 0, sizeof(virtio_device_t));
    vdev->pci_dev = pci_dev;

    /* Enable Bus Mastering and IO/Memory decoding on the PCI device */
    pci_enable_bus_mastering(pci_dev);

    /* Check for Modern Virtio PCI capabilities */
    for (pci_cap_t *cap = pci_dev->caps; cap != NULL; cap = cap->next) {
        if (cap->id != PCI_CAP_ID_VNDR) continue;

        uint8_t offset = cap->offset;
        if (offset > 240) continue;
        uint8_t length = pci_read_config8(pci_dev->bus, pci_dev->slot, pci_dev->func, (uint8_t)(offset + 2));
        if (length < 16) continue;
        uint8_t cfg_type = pci_read_config8(pci_dev->bus, pci_dev->slot, pci_dev->func, (uint8_t)(offset + 3));
        uint8_t bar = pci_read_config8(pci_dev->bus, pci_dev->slot, pci_dev->func, (uint8_t)(offset + 4));
        uint32_t bar_offset = pci_read_config32(pci_dev->bus, pci_dev->slot, pci_dev->func, (uint8_t)(offset + 8));

        if (bar >= 6 || pci_dev->bars[bar].is_io || !pci_dev->bars[bar].base_virt) continue;
        uint32_t bar_length = pci_read_config32(pci_dev->bus, pci_dev->slot, pci_dev->func, (uint8_t)(offset + 12));
        if (!bar_length || bar_offset > pci_dev->bars[bar].size ||
            bar_length > pci_dev->bars[bar].size - bar_offset) continue;

        uint8_t *bar_base = (uint8_t *)pci_dev->bars[bar].base_virt;

        switch (cfg_type) {
        case VIRTIO_PCI_CAP_COMMON_CFG:
            if (bar_length >= sizeof(*vdev->common_cfg)) {
                vdev->common_cfg = (volatile virtio_pci_common_cfg_t *)(bar_base + bar_offset);
            }
            break;
        case VIRTIO_PCI_CAP_NOTIFY_CFG:
            if (length < 20 || offset > 236 || bar_length < sizeof(uint16_t)) break;
            vdev->notify_base = (volatile uint8_t *)(bar_base + bar_offset);
            vdev->notify_length = bar_length;
            vdev->notify_mult = pci_read_config32(pci_dev->bus, pci_dev->slot, pci_dev->func, (uint8_t)(offset + 16));
            break;
        case VIRTIO_PCI_CAP_ISR_CFG:
            vdev->isr_cfg = (volatile uint8_t *)(bar_base + bar_offset);
            break;
        case VIRTIO_PCI_CAP_DEVICE_CFG:
            vdev->device_cfg = (void *)(bar_base + bar_offset);
            break;
        }
    }

    if (vdev->common_cfg && vdev->notify_base) {
        vdev->is_modern = true;
        kprintf("[VIRTIO] Probed modern PCI virtio device at %02x:%02x.%x\n",
                pci_dev->bus, pci_dev->slot, pci_dev->func);
    } else if (pci_dev->bars[0].is_io) {
        vdev->is_modern = false;
        vdev->io_base = (uint16_t)pci_dev->bars[0].base_phys;
        vdev->device_cfg = (void *)(uintptr_t)(vdev->io_base + 0x14);
        kprintf("[VIRTIO] Probed legacy PCI virtio device at %02x:%02x.%x (io_base=0x%x)\n",
                pci_dev->bus, pci_dev->slot, pci_dev->func, vdev->io_base);
    } else {
        kprintf("[VIRTIO] ERROR: Device at %02x:%02x.%x is neither modern nor legacy I/O virtio!\n",
                pci_dev->bus, pci_dev->slot, pci_dev->func);
        return -STRATUM_ENODEV;
    }

    return virtio_reset_device(vdev);
}

int virtio_reset_device(virtio_device_t *vdev) {
    if (!vdev) return -STRATUM_EINVAL;

    virtio_set_status(vdev, VIRTIO_STATUS_RESET);
    unsigned attempts = 1000000;
    while (virtio_get_status(vdev) != VIRTIO_STATUS_RESET) {
        if (!--attempts) return -STRATUM_ETIMEDOUT;
        cpu_pause();
    }

    /* A zero status confirms the device has stopped touching the old queues. */
    for (unsigned i = 0; i < VIRTIO_MAX_QUEUES; i++) {
        virtqueue_destroy(vdev->queues[i]);
        vdev->queues[i] = NULL;
        vdev->doorbells[i] = NULL;
    }
    vdev->num_queues = 0;
    vdev->device_features = 0;
    vdev->negotiated_features = 0;
    virtio_set_status(vdev, VIRTIO_STATUS_ACKNOWLEDGE);
    virtio_set_status(vdev, VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);
    return 0;
}

int virtio_negotiate_features(virtio_device_t *vdev, uint64_t required_features) {
    if (!vdev) return -STRATUM_EINVAL;

    if (vdev->is_modern) {
        vdev->common_cfg->device_feature_select = 0;
        uint32_t f0 = vdev->common_cfg->device_feature;
        vdev->common_cfg->device_feature_select = 1;
        uint32_t f1 = vdev->common_cfg->device_feature;
        vdev->device_features = ((uint64_t)f1 << 32) | f0;

        if (!(vdev->device_features & VIRTIO_F_VERSION_1)) {
            virtio_set_status(vdev, VIRTIO_STATUS_FAILED);
            return -STRATUM_ENOTSUP;
        }
        uint64_t supported = vdev->device_features & (required_features | VIRTIO_F_VERSION_1);

        vdev->common_cfg->driver_feature_select = 0;
        vdev->common_cfg->driver_feature = (uint32_t)(supported & 0xFFFFFFFF);
        vdev->common_cfg->driver_feature_select = 1;
        vdev->common_cfg->driver_feature = (uint32_t)(supported >> 32);

        vdev->negotiated_features = supported;

        /* Set FEATURES_OK */
        uint8_t s = virtio_get_status(vdev);
        virtio_set_status(vdev, s | VIRTIO_STATUS_FEATURES_OK);

        if (!(virtio_get_status(vdev) & VIRTIO_STATUS_FEATURES_OK)) {
            virtio_set_status(vdev, VIRTIO_STATUS_FAILED);
            kprintf("[VIRTIO] Device rejected negotiated features!\n");
            return -STRATUM_EINVAL;
        }
    } else {
        uint32_t f0 = inl(vdev->io_base + 0);
        vdev->device_features = f0;
        uint32_t supported = (uint32_t)(vdev->device_features & required_features);

        outl(vdev->io_base + 4, supported);
        vdev->negotiated_features = supported;
    }

    return 0;
}

virtqueue_t *virtio_setup_queue(virtio_device_t *vdev, uint16_t queue_idx) {
    if (!vdev || queue_idx >= VIRTIO_MAX_QUEUES) return NULL;

    uint16_t queue_size = 0;
    virtqueue_notify_fn notify_fn = vdev->is_modern ? virtio_notify_modern : virtio_notify_legacy;

    if (vdev->is_modern) {
        vdev->common_cfg->queue_select = queue_idx;
        queue_size = vdev->common_cfg->queue_size;
        uint64_t offset = (uint64_t)vdev->common_cfg->queue_notify_off * vdev->notify_mult;
        if (queue_size == 0 || offset > vdev->notify_length - sizeof(uint16_t)) return NULL;

        virtqueue_t *vq = virtqueue_create(queue_idx, queue_size, notify_fn, vdev);
        if (!vq) return NULL;

        uint64_t desc_phys = vq->mem_phys;
        uint64_t driver_phys = vq->mem_phys + (uint64_t)((uint8_t *)vq->avail - (uint8_t *)vq->desc);
        uint64_t device_phys = vq->mem_phys + (uint64_t)((uint8_t *)vq->used - (uint8_t *)vq->desc);

        vdev->doorbells[queue_idx] = (volatile uint16_t *)(vdev->notify_base + offset);
        vdev->common_cfg->queue_desc = desc_phys;
        vdev->common_cfg->queue_driver = driver_phys;
        vdev->common_cfg->queue_device = device_phys;
        vdev->common_cfg->queue_enable = 1;

        vdev->queues[queue_idx] = vq;
        vdev->num_queues++;
        return vq;
    } else {
        outw((uint16_t)(vdev->io_base + 0x0E), queue_idx);
        queue_size = inw((uint16_t)(vdev->io_base + 0x0C));
        if (queue_size == 0) return NULL;

        virtqueue_t *vq = virtqueue_create(queue_idx, queue_size, notify_fn, vdev);
        if (!vq) return NULL;

        /* Legacy queue address is physical PFN (paddr / 4096) */
        outl((uint16_t)(vdev->io_base + 0x08), (uint32_t)(vq->mem_phys / PAGE_SIZE));

        vdev->queues[queue_idx] = vq;
        vdev->num_queues++;
        return vq;
    }
}

bool virtio_queue_msix(virtio_device_t *vdev, uint16_t queue_idx, uint8_t vector, uint8_t apic_id) {
    if (!vdev->is_modern || queue_idx >= VIRTIO_MAX_QUEUES || !vdev->queues[queue_idx]) return false;
    if (!pci_configure_msix(vdev->pci_dev, 0, vector, apic_id)) return false;
    vdev->common_cfg->queue_select = queue_idx;
    vdev->common_cfg->queue_msix_vector = 0;
    if (vdev->common_cfg->queue_msix_vector == 0) return true;
    pci_disable_msix(vdev->pci_dev);
    return false;
}

void virtio_driver_ok(virtio_device_t *vdev) {
    if (!vdev) return;
    uint8_t s = virtio_get_status(vdev);
    virtio_set_status(vdev, s | VIRTIO_STATUS_DRIVER_OK);
    kprintf("[VIRTIO] Driver OK set for device at %02x:%02x.%x\n",
            vdev->pci_dev->bus, vdev->pci_dev->slot, vdev->pci_dev->func);
}
