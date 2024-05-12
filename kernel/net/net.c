#include <kernel/net.h>
#include <kernel/sched.h>
#include <kernel/event.h>
#include <kernel/boot.h>
#include <kernel/ethernet.h>
#include <kernel/arp.h>
#include <kernel/ipv4.h>
#include <kernel/icmp.h>
#include <kernel/udp.h>
#include <kernel/tcp.h>
#include <kernel/socket.h>
#include <kernel/virtio_net.h>
#include <kernel/slab.h>
#include <kernel/kernel.h>
#include <kernel/string.h>
#include <shared/errno.h>

static kmem_cache_t *g_pbuf_cache = NULL;
static netif_t       g_default_netif;
static bool          g_net_initialized = false;

pbuf_t *pbuf_alloc(size_t size) {
    if (size > PBUF_MAX_SIZE) return NULL;
    if (!g_pbuf_cache) return NULL;

    pbuf_t *p = (pbuf_t *)kmem_cache_alloc(g_pbuf_cache);
    if (!p) return NULL;

    memset(p, 0, sizeof(pbuf_t));
    p->payload = p->data;
    p->len = (uint16_t)size;
    p->tot_len = (uint16_t)size;
    p->refcount = 1;
    p->next = NULL;

    return p;
}

void pbuf_ref(pbuf_t *p) {
    if (!p) return;
    __atomic_add_fetch(&p->refcount, 1, __ATOMIC_SEQ_CST);
}

void pbuf_free(pbuf_t *p) {
    if (!p) return;
    if (__atomic_sub_fetch(&p->refcount, 1, __ATOMIC_SEQ_CST) == 0) {
        kmem_cache_free(g_pbuf_cache, p);
    }
}

static int virtio_net_output(netif_t *nif, pbuf_t *p, const uint8_t *dst_mac, uint16_t ethertype) {
    if (!nif || !p || !dst_mac) return -STRATUM_EINVAL;
    virtio_net_dev_t *dev = (virtio_net_dev_t *)nif->driver_priv;
    if (!dev) return -STRATUM_ENODEV;

    /* Build Ethernet Frame */
    uint8_t frame[ETH_FRAME_LEN];
    eth_hdr_t *eh = (eth_hdr_t *)frame;
    memcpy(eh->dest, dst_mac, ETH_ALEN);
    memcpy(eh->src, nif->mac, ETH_ALEN);
    eh->type = htons(ethertype);

    size_t payload_len = p->tot_len;
    if (ETH_HLEN + payload_len > ETH_FRAME_LEN) {
        return -STRATUM_EMSGSIZE;
    }

    memcpy(frame + ETH_HLEN, p->payload, payload_len);
    size_t total_len = ETH_HLEN + payload_len;
    if (total_len < 60) {
        memset(frame + total_len, 0, 60 - total_len);
        total_len = 60;
    }

    int res = virtio_net_transmit(dev, frame, total_len);
    return (res > 0) ? 0 : res;
}

netif_t *net_get_default_if(void) {
    return &g_default_netif;
}

void net_poll(void) {
    virtio_net_dev_t *dev = (virtio_net_dev_t *)g_default_netif.driver_priv;
    if (!dev) return;

    tcp_tick();
    static uint8_t rx_raw[2048];
    bool processed = false;
    for (unsigned budget = 0; budget < 32; budget++) {
        int bytes = virtio_net_receive(dev, rx_raw, sizeof(rx_raw));
        if (bytes <= 0) break;
        if ((size_t)bytes < ETH_HLEN) continue;

        pbuf_t *p = pbuf_alloc((size_t)bytes);
        if (!p) continue;

        memcpy(p->data, rx_raw, (size_t)bytes);
        p->len = (uint16_t)bytes;
        p->tot_len = (uint16_t)bytes;

        ethernet_input(p);
        processed = true;
    }
    if (processed) readiness_notify();
}

static void net_worker(void *arg) {
    (void)arg;
    for (;;) {
        net_poll();
        thread_sleep_ms(1);
    }
}

void net_start_worker(void) {
    if (g_default_netif.driver_priv &&
        !thread_create_on_cpu("net-rx", net_worker, NULL, THREAD_PRIO_HIGH, 0)) {
        kprintf("[NET] Failed to start the receive worker.\n");
    }
}

static uint32_t boot_ip(void) {
    const char *arg = g_boot_handoff.cmdline;
    while (*arg) {
        while (*arg == ' ' || *arg == '\n' || *arg == '\r') arg++;
        if (strncmp(arg, "ip=", 3) == 0) {
            const char *p = arg + 3;
            uint32_t address = 0;
            for (unsigned i = 0; i < 4; i++) {
                unsigned octet = 0;
                unsigned digits = 0;
                while (*p >= '0' && *p <= '9' && digits < 3) {
                    octet = octet * 10 + (unsigned)(*p++ - '0');
                    digits++;
                }
                if (!digits || octet > 255 || (i < 3 && *p++ != '.')) break;
                address = (address << 8) | octet;
                if (i == 3 && (!*p || *p == ' ' || *p == '\n' || *p == '\r')) return address;
            }
        }
        while (*arg && *arg != ' ' && *arg != '\n' && *arg != '\r') arg++;
    }
    return IP4_ADDR(10, 0, 2, 15);
}

void net_init(void) {
    if (g_net_initialized) return;

    g_pbuf_cache = kmem_cache_create("pbuf_cache", sizeof(pbuf_t), 8);
    kassert(g_pbuf_cache != NULL);

    memset(&g_default_netif, 0, sizeof(netif_t));
    strcpy(g_default_netif.name, "eth0");

    virtio_net_dev_t *dev = virtio_net_get_primary();
    if (dev) {
        memcpy(g_default_netif.mac, dev->mac, ETH_ALEN);
        g_default_netif.driver_priv = dev;
        g_default_netif.output = virtio_net_output;
    }

    /* QEMU User Networking defaults: Guest is 10.0.2.15, Gateway is 10.0.2.2 */
    g_default_netif.ip_addr = boot_ip();
    g_default_netif.netmask = IP4_ADDR(255, 255, 255, 0);
    g_default_netif.gateway = IP4_ADDR(10, 0, 2, 2);

    arp_init();
    ipv4_init();
    icmp_init();
    udp_init();
    tcp_init();
    socket_init();

    g_net_initialized = true;
    kprintf("[NET] Network subsystem initialized: iface=%s ip=%u.%u.%u.%u mac=%02x:%02x:%02x:%02x:%02x:%02x\n",
            g_default_netif.name,
            g_default_netif.ip_addr >> 24, (g_default_netif.ip_addr >> 16) & 255U,
            (g_default_netif.ip_addr >> 8) & 255U, g_default_netif.ip_addr & 255U,
            g_default_netif.mac[0], g_default_netif.mac[1], g_default_netif.mac[2],
            g_default_netif.mac[3], g_default_netif.mac[4], g_default_netif.mac[5]);
}
