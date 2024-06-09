#ifndef STRATUM_KERNEL_VIRTIO_H
#define STRATUM_KERNEL_VIRTIO_H

#include <kernel/types.h>
#include <kernel/pci.h>
#include <kernel/virtqueue.h>

/* Virtio Device Status Bits */
#define VIRTIO_STATUS_RESET         0x00
#define VIRTIO_STATUS_ACKNOWLEDGE   0x01
#define VIRTIO_STATUS_DRIVER        0x02
#define VIRTIO_STATUS_DRIVER_OK     0x04
#define VIRTIO_STATUS_FEATURES_OK   0x08
#define VIRTIO_STATUS_FAILED        0x80

/* Modern PCI Capability Types */
#define VIRTIO_PCI_CAP_COMMON_CFG   1
#define VIRTIO_PCI_CAP_NOTIFY_CFG   2
#define VIRTIO_PCI_CAP_ISR_CFG      3
#define VIRTIO_PCI_CAP_DEVICE_CFG   4
#define VIRTIO_PCI_CAP_PCI_CFG      5

/* Generic Feature Bits */
#define VIRTIO_F_VERSION_1          (1ULL << 32)
#define VIRTIO_F_ANY_LAYOUT         (1ULL << 27)

/* Modern PCI Capability Header */
typedef struct virtio_pci_cap {
    uint8_t  cap_vndr;
    uint8_t  cap_next;
    uint8_t  cap_len;
    uint8_t  cfg_type;
    uint8_t  bar;
    uint8_t  padding[3];
    uint32_t offset;
    uint32_t length;
} __attribute__((packed)) virtio_pci_cap_t;

typedef struct virtio_pci_notify_cap {
    virtio_pci_cap_t cap;
    uint32_t         notify_off_multiplier;
} __attribute__((packed)) virtio_pci_notify_cap_t;

typedef struct virtio_pci_common_cfg {
    uint32_t device_feature_select;
    uint32_t device_feature;
    uint32_t driver_feature_select;
    uint32_t driver_feature;
    uint16_t msix_config;
    uint16_t num_queues;
    uint8_t  device_status;
    uint8_t  config_generation;

    uint16_t queue_select;
    uint16_t queue_size;
    uint16_t queue_msix_vector;
    uint16_t queue_enable;
    uint16_t queue_notify_off;
    uint64_t queue_desc;
    uint64_t queue_driver;
    uint64_t queue_device;
} __attribute__((packed)) virtio_pci_common_cfg_t;

#define VIRTIO_MAX_QUEUES 8

typedef struct virtio_device {
    pci_device_t                    *pci_dev;
    bool                             is_modern;

    /* Modern MMIO structures */
    volatile virtio_pci_common_cfg_t *common_cfg;
    volatile uint8_t                *notify_base;
    uint32_t                         notify_mult;
    uint32_t                         notify_length;
    volatile uint16_t              *doorbells[VIRTIO_MAX_QUEUES];
    volatile uint8_t                *isr_cfg;
    void                            *device_cfg;

    /* Legacy I/O Port base */
    uint16_t                         io_base;

    uint64_t                         device_features;
    uint64_t                         negotiated_features;

    virtqueue_t                     *queues[VIRTIO_MAX_QUEUES];
    uint16_t                         num_queues;
} virtio_device_t;

int          virtio_probe_device(pci_device_t *pci_dev, virtio_device_t *vdev);
int          virtio_reset_device(virtio_device_t *vdev);
int          virtio_negotiate_features(virtio_device_t *vdev, uint64_t required_features);
virtqueue_t *virtio_setup_queue(virtio_device_t *vdev, uint16_t queue_idx);
bool         virtio_queue_msix(virtio_device_t *vdev, uint16_t queue_idx, uint8_t vector, uint8_t apic_id);
void         virtio_set_status(virtio_device_t *vdev, uint8_t status);
uint8_t      virtio_get_status(virtio_device_t *vdev);
void         virtio_driver_ok(virtio_device_t *vdev);

#endif /* STRATUM_KERNEL_VIRTIO_H */
