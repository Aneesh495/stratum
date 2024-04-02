#include <kernel/virtio_net.h>
#include <kernel/pmm.h>
#include <kernel/slab.h>
#include <kernel/kernel.h>
#include <kernel/string.h>
#include <kernel/x86_64.h>
#include <shared/errno.h>

static virtio_net_dev_t g_primary_net;
static bool             g_primary_net_registered = false;

/* Modern virtio always includes num_buffers in its header. Legacy omits it
 * when the mergeable receive buffer feature is disabled. */
static size_t header_size(const virtio_net_dev_t *dev) {
    return dev->vdev.is_modern ? sizeof(virtio_net_hdr_t) : sizeof(virtio_net_hdr_t) - 2;
}

virtio_net_dev_t *virtio_net_get_primary(void) {
    return g_primary_net_registered ? &g_primary_net : NULL;
}

static int virtio_net_enqueue_rx(virtio_net_dev_t *dev, virtio_net_rx_buf_t *rx_buf) {
    vring_buf_t buf;
    buf.phys_addr = virt_to_phys(rx_buf);
    buf.len = (uint32_t)(header_size(dev) + VIRTIO_NET_MAX_FRAME);
    buf.is_write = true;

    return virtqueue_add_buf(dev->rx_vq, &buf, 0, 1, rx_buf);
}

int virtio_net_init(void) {
    pci_device_t *pci_dev = pci_find_device(0x1AF4, VIRTIO_NET_PCI_MODERN_DEVICE_ID);
    if (!pci_dev) {
        pci_dev = pci_find_device(0x1AF4, VIRTIO_NET_PCI_LEGACY_DEVICE_ID);
    }

    if (!pci_dev) {
        kprintf("[VIRTIO-NET] No Virtio-net PCI device detected.\n");
        return -STRATUM_ENODEV;
    }

    virtio_net_dev_t *dev = &g_primary_net;
    memset(dev, 0, sizeof(virtio_net_dev_t));
    spin_lock_init(&dev->rx_lock);
    spin_lock_init(&dev->tx_lock);

    int res = virtio_probe_device(pci_dev, &dev->vdev);
    if (res != 0) {
        kprintf("[VIRTIO-NET] Failed to probe virtio device: %d\n", res);
        return res;
    }

    /* Negotiate supported features */
    uint64_t features = VIRTIO_NET_F_MAC | VIRTIO_NET_F_STATUS;
    res = virtio_negotiate_features(&dev->vdev, features);
    if (res != 0) return res;

    /* Setup RX queue (0) and TX queue (1) */
    dev->rx_vq = virtio_setup_queue(&dev->vdev, 0);
    if (!dev->rx_vq) {
        kprintf("[VIRTIO-NET] Failed to setup RX virtqueue 0!\n");
        return -STRATUM_ENOMEM;
    }

    dev->tx_vq = virtio_setup_queue(&dev->vdev, 1);
    if (!dev->tx_vq) {
        kprintf("[VIRTIO-NET] Failed to setup TX virtqueue 1!\n");
        return -STRATUM_ENOMEM;
    }

    /* Read MAC address */
    if (dev->vdev.is_modern && dev->vdev.device_cfg) {
        volatile virtio_net_config_t *cfg = (volatile virtio_net_config_t *)dev->vdev.device_cfg;
        for (int i = 0; i < VIRTIO_NET_ETH_ALEN; i++) {
            dev->mac[i] = cfg->mac[i];
        }
        dev->link_up = (cfg->status & VIRTIO_NET_S_LINK_UP) != 0;
    } else {
        for (int i = 0; i < VIRTIO_NET_ETH_ALEN; i++) {
            dev->mac[i] = inb((uint16_t)(dev->vdev.io_base + 0x14 + i));
        }
        dev->link_up = true;
    }

    /* Populate RX buffers */
    for (int i = 0; i < VIRTIO_NET_RX_BUFFERS; i++) {
        virtio_net_rx_buf_t *rx_buf = (virtio_net_rx_buf_t *)kmalloc(sizeof(virtio_net_rx_buf_t));
        if (!rx_buf) break;
        memset(rx_buf, 0, sizeof(virtio_net_rx_buf_t));
        dev->rx_pool[i] = rx_buf;
        virtio_net_enqueue_rx(dev, rx_buf);
    }
    /* Enable DMA only once all queues have been configured. */
    virtio_driver_ok(&dev->vdev);
    virtqueue_kick(dev->rx_vq);
    g_primary_net_registered = true;

    kprintf("[VIRTIO-NET] Network device initialized: MAC=%02x:%02x:%02x:%02x:%02x:%02x (link=%u)\n",
            dev->mac[0], dev->mac[1], dev->mac[2], dev->mac[3], dev->mac[4], dev->mac[5],
            dev->link_up);

    return 0;
}

int virtio_net_transmit(virtio_net_dev_t *dev, const void *packet, size_t len) {
    if (!dev || !dev->tx_vq || !packet) return -STRATUM_EINVAL;
    if (len == 0 || len > VIRTIO_NET_MAX_FRAME) return -STRATUM_EINVAL;

    uint64_t flags;
    spin_lock_irqsave(&dev->tx_lock, &flags);
    if (dev->tx_failed) {
        spin_unlock_irqrestore(&dev->tx_lock, flags);
        return -STRATUM_EIO;
    }

    /* Allocate header and packet copy */
    virtio_net_hdr_t *hdr = (virtio_net_hdr_t *)kmalloc(sizeof(virtio_net_hdr_t));
    if (!hdr) {
        spin_unlock_irqrestore(&dev->tx_lock, flags);
        return -STRATUM_ENOMEM;
    }
    memset(hdr, 0, sizeof(virtio_net_hdr_t));

    void *pkt_buf = kmalloc(len);
    if (!pkt_buf) {
        kfree(hdr);
        spin_unlock_irqrestore(&dev->tx_lock, flags);
        return -STRATUM_ENOMEM;
    }
    memcpy(pkt_buf, packet, len);

    vring_buf_t bufs[2];
    bufs[0].phys_addr = virt_to_phys(hdr);
    bufs[0].len = (uint32_t)header_size(dev);
    bufs[0].is_write = false;

    bufs[1].phys_addr = virt_to_phys(pkt_buf);
    bufs[1].len = (uint32_t)len;
    bufs[1].is_write = false;

    int res = virtqueue_add_buf(dev->tx_vq, bufs, 2, 0, hdr);
    if (res != 0) {
        kfree(pkt_buf);
        kfree(hdr);
        spin_unlock_irqrestore(&dev->tx_lock, flags);
        return res;
    }

    virtqueue_kick(dev->tx_vq);

    /* Poll for completion */
    uint64_t timeout_iters = 5000000ULL;
    void *completed = NULL;
    while (timeout_iters-- > 0) {
        completed = virtqueue_get_buf(dev->tx_vq, NULL);
        if (completed != NULL) {
            break;
        }
        __asm__ volatile("pause");
    }

    if (!completed) {
        /* DMA may still be in flight. Quarantine this queue and its buffers. */
        dev->tx_failed = true;
        spin_unlock_irqrestore(&dev->tx_lock, flags);
        kprintf("[VIRTIO-NET] Warning: TX packet timed out!\n");
        return -STRATUM_ETIMEDOUT;
    }

    kfree(pkt_buf);
    kfree(hdr);
    spin_unlock_irqrestore(&dev->tx_lock, flags);
    return (int)len;
}

int virtio_net_receive(virtio_net_dev_t *dev, void *buf, size_t max_len) {
    if (!dev || !dev->rx_vq || !buf) return -STRATUM_EINVAL;

    uint64_t flags;
    spin_lock_irqsave(&dev->rx_lock, &flags);

    uint32_t len = 0;
    virtio_net_rx_buf_t *rx_buf = (virtio_net_rx_buf_t *)virtqueue_get_buf(dev->rx_vq, &len);
    if (!rx_buf) {
        spin_unlock_irqrestore(&dev->rx_lock, flags);
        return 0; /* No packet waiting */
    }

    int pkt_len = 0;
    size_t header = header_size(dev);
    if (len > header && len <= header + VIRTIO_NET_MAX_FRAME) {
        pkt_len = (int)(len - header);
        if ((size_t)pkt_len > max_len) {
            pkt_len = (int)max_len;
        }
        memcpy(buf, (const uint8_t *)rx_buf + header, (size_t)pkt_len);
    }

    /* Replenish RX queue */
    virtio_net_enqueue_rx(dev, rx_buf);
    virtqueue_kick(dev->rx_vq);

    spin_unlock_irqrestore(&dev->rx_lock, flags);
    return pkt_len;
}
