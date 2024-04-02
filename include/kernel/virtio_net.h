#ifndef STRATUM_KERNEL_VIRTIO_NET_H
#define STRATUM_KERNEL_VIRTIO_NET_H

#include <kernel/types.h>
#include <kernel/virtio.h>

#define VIRTIO_NET_PCI_LEGACY_DEVICE_ID 0x1000
#define VIRTIO_NET_PCI_MODERN_DEVICE_ID 0x1041

#define VIRTIO_NET_F_CSUM       (1ULL << 0)
#define VIRTIO_NET_F_GUEST_CSUM (1ULL << 1)
#define VIRTIO_NET_F_MAC        (1ULL << 5)
#define VIRTIO_NET_F_STATUS     (1ULL << 16)

#define VIRTIO_NET_S_LINK_UP    1

#define VIRTIO_NET_ETH_ALEN     6
#define VIRTIO_NET_MAX_FRAME    1514
#define VIRTIO_NET_RX_BUFFERS   32

typedef struct virtio_net_config {
    uint8_t  mac[VIRTIO_NET_ETH_ALEN];
    uint16_t status;
    uint16_t max_virtqueue_pairs;
    uint16_t mtu;
} __attribute__((packed)) virtio_net_config_t;

typedef struct virtio_net_hdr {
    uint8_t  flags;
    uint8_t  gso_type;
    uint16_t hdr_len;
    uint16_t gso_size;
    uint16_t csum_start;
    uint16_t csum_offset;
    uint16_t num_buffers;
} __attribute__((packed)) virtio_net_hdr_t;

typedef struct virtio_net_rx_buf {
    virtio_net_hdr_t hdr;
    uint8_t          packet[VIRTIO_NET_MAX_FRAME];
} __attribute__((aligned(16))) virtio_net_rx_buf_t;

typedef struct virtio_net_dev {
    virtio_device_t       vdev;
    virtqueue_t          *rx_vq;
    virtqueue_t          *tx_vq;
    uint8_t               mac[VIRTIO_NET_ETH_ALEN];
    bool                  link_up;

    virtio_net_rx_buf_t  *rx_pool[VIRTIO_NET_RX_BUFFERS];
    spinlock_t            rx_lock;
    spinlock_t            tx_lock;
    bool                  tx_failed;
} virtio_net_dev_t;

int               virtio_net_init(void);
virtio_net_dev_t *virtio_net_get_primary(void);
int               virtio_net_transmit(virtio_net_dev_t *dev, const void *packet, size_t len);
int               virtio_net_receive(virtio_net_dev_t *dev, void *buf, size_t max_len);

#endif /* STRATUM_KERNEL_VIRTIO_NET_H */
