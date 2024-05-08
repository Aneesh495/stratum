#include <kernel/spinlock.h>
#include <kernel/udp.h>
#include <kernel/ipv4.h>
#include <kernel/socket.h>
#include <kernel/slab.h>
#include <kernel/string.h>
#include <shared/errno.h>

static udp_pcb_t  *g_udp_pcbs = NULL;
static spinlock_t  g_udp_lock;
static uint16_t    g_udp_ephemeral_port = 49152;

void udp_init(void) {
    spin_lock_init(&g_udp_lock);
    g_udp_pcbs = NULL;
    g_udp_ephemeral_port = 49152;
}

udp_pcb_t *udp_new(void) {
    udp_pcb_t *pcb = (udp_pcb_t *)kmalloc(sizeof(udp_pcb_t));
    if (!pcb) return NULL;

    memset(pcb, 0, sizeof(udp_pcb_t));

    uint64_t flags;
    spin_lock_irqsave(&g_udp_lock, &flags);
    pcb->next = g_udp_pcbs;
    g_udp_pcbs = pcb;
    spin_unlock_irqrestore(&g_udp_lock, flags);

    return pcb;
}

static bool port_available(const udp_pcb_t *pcb, uint16_t port) {
    for (const udp_pcb_t *other = g_udp_pcbs; other; other = other->next) {
        if (other != pcb && other->local_port == port) return false;
    }
    return true;
}

static int bind_port(udp_pcb_t *pcb, uint16_t port) {
    if (pcb->local_port) return -STRATUM_EINVAL;
    if (!port) {
        for (unsigned i = 0; i < 16384; i++) {
            uint16_t candidate = g_udp_ephemeral_port++;
            if (g_udp_ephemeral_port == 0) g_udp_ephemeral_port = 49152;
            if (port_available(pcb, candidate)) {
                port = candidate;
                break;
            }
        }
    }
    if (!port || !port_available(pcb, port)) return -STRATUM_EADDRINUSE;
    pcb->local_port = port;
    return 0;
}

int udp_bind(udp_pcb_t *pcb, uint16_t port) {
    uint64_t flags;
    spin_lock_irqsave(&g_udp_lock, &flags);
    int res = bind_port(pcb, port);
    spin_unlock_irqrestore(&g_udp_lock, flags);
    return res;
}

int udp_connect(udp_pcb_t *pcb, uint32_t ip, uint16_t port) {
    if (!ip || !port) return -STRATUM_EINVAL;
    uint64_t flags;
    spin_lock_irqsave(&g_udp_lock, &flags);
    int res = pcb->local_port ? 0 : bind_port(pcb, 0);
    if (res == 0) {
        pcb->remote_ip = ip;
        pcb->remote_port = port;
    }
    spin_unlock_irqrestore(&g_udp_lock, flags);
    return res;
}

void udp_close(udp_pcb_t *pcb) {
    if (!pcb) return;

    uint64_t flags;
    spin_lock_irqsave(&g_udp_lock, &flags);

    udp_pcb_t *prev = NULL;
    udp_pcb_t *cur = g_udp_pcbs;
    while (cur) {
        if (cur == pcb) {
            if (prev) prev->next = cur->next;
            else g_udp_pcbs = cur->next;
            break;
        }
        prev = cur;
        cur = cur->next;
    }

    spin_unlock_irqrestore(&g_udp_lock, flags);
    while (pcb->rx_head) {
        pbuf_t *p = pcb->rx_head;
        pcb->rx_head = p->next;
        pbuf_free(p);
    }
    kfree(pcb);
}

int udp_output(uint32_t src_ip, uint16_t src_port, uint32_t dst_ip, uint16_t dst_port,
               const void *data, size_t len) {
    if (len > UDP_MAX_PAYLOAD) return -STRATUM_EMSGSIZE;

    size_t udp_len = sizeof(udp_hdr_t) + len;
    pbuf_t *p = pbuf_alloc(udp_len);
    if (!p) return -STRATUM_ENOMEM;

    udp_hdr_t *uh = (udp_hdr_t *)p->payload;
    uh->src_port = htons(src_port);
    uh->dst_port = htons(dst_port);
    uh->length = htons((uint16_t)udp_len);
    uh->checksum = 0;

    if (data && len > 0) {
        memcpy(p->payload + sizeof(udp_hdr_t), data, len);
    }

    netif_t *nif = net_get_default_if();
    uint32_t sip = (src_ip != 0) ? src_ip : (nif ? nif->ip_addr : 0);
    uh->checksum = net_pseudo_checksum(sip, dst_ip, IP_PROTO_UDP, p->payload, udp_len);

    if (!uh->checksum) uh->checksum = 0xFFFF;
    int res = ipv4_output(p, dst_ip, IP_PROTO_UDP);
    pbuf_free(p);
    return res;
}

int udp_input(pbuf_t *p, uint32_t src_ip, uint32_t dst_ip) {
    if (!p || p->len < sizeof(udp_hdr_t)) return -STRATUM_EINVAL;

    const udp_hdr_t *hdr = (const udp_hdr_t *)p->payload;
    uint16_t dst_port = ntohs(hdr->dst_port);
    uint16_t src_port = ntohs(hdr->src_port);
    uint16_t length = ntohs(hdr->length);
    if (!dst_port || length < sizeof(*hdr) || length != p->len) return -STRATUM_EINVAL;
    if (hdr->checksum && net_pseudo_checksum(src_ip, dst_ip, IP_PROTO_UDP, p->payload, length)) {
        return -STRATUM_EIO;
    }

    uint64_t flags;
    spin_lock_irqsave(&g_udp_lock, &flags);
    int res = -STRATUM_ECONNREFUSED;
    for (udp_pcb_t *pcb = g_udp_pcbs; pcb; pcb = pcb->next) {
        if (pcb->local_port != dst_port ||
            (pcb->remote_port && (pcb->remote_port != src_port || pcb->remote_ip != src_ip))) continue;
        if (pcb->rx_count == 8) {
            res = -STRATUM_ENOBUFS;
            break;
        }
        p->payload += sizeof(*hdr);
        p->len = length - sizeof(*hdr);
        pbuf_ref(p);
        if (pcb->rx_tail) pcb->rx_tail->next = p;
        else pcb->rx_head = p;
        pcb->rx_tail = p;
        pcb->rx_count++;
        res = 0;
        break;
    }
    spin_unlock_irqrestore(&g_udp_lock, flags);
    return res;
}

int64_t udp_send(udp_pcb_t *pcb, const void *buf, size_t len, uint32_t ip, uint16_t port) {
    uint64_t flags;
    spin_lock_irqsave(&g_udp_lock, &flags);
    if (!port) {
        ip = pcb->remote_ip;
        port = pcb->remote_port;
    }
    uint16_t local = pcb->local_port;
    spin_unlock_irqrestore(&g_udp_lock, flags);
    if (!port || !local) return -STRATUM_ENOTCONN;
    int res = udp_output(0, local, ip, port, buf, len);
    return res ? res : (int64_t)len;
}

int64_t udp_recv(udp_pcb_t *pcb, void *buf, size_t len) {
    uint64_t flags;
    spin_lock_irqsave(&g_udp_lock, &flags);
    pbuf_t *p = pcb->rx_head;
    if (!p) {
        spin_unlock_irqrestore(&g_udp_lock, flags);
        return -STRATUM_EAGAIN;
    }
    pcb->rx_head = p->next;
    if (!pcb->rx_head) pcb->rx_tail = NULL;
    pcb->rx_count--;
    spin_unlock_irqrestore(&g_udp_lock, flags);
    size_t n = len < p->len ? len : p->len;
    memcpy(buf, p->payload, n);
    pbuf_free(p);
    return (int64_t)n;
}

int udp_poll(udp_pcb_t *pcb, uint32_t events) {
    uint64_t flags;
    spin_lock_irqsave(&g_udp_lock, &flags);
    int res = 0;
    if (pcb->rx_head) res |= events & POLLIN;
    if (pcb->remote_port) res |= events & POLLOUT;
    spin_unlock_irqrestore(&g_udp_lock, flags);
    return res;
}
