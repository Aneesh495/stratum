#include <kernel/ipv4.h>
#include <kernel/ethernet.h>
#include <kernel/arp.h>
#include <kernel/icmp.h>
#include <kernel/udp.h>
#include <kernel/tcp.h>
#include <kernel/string.h>
#include <shared/errno.h>

static uint16_t g_ip_id_counter = 1;

void ipv4_init(void) {
    g_ip_id_counter = 1;
}

int ipv4_input(pbuf_t *p) {
    if (!p) return -STRATUM_EINVAL;
    ipv4_view_t view;
    int result = ipv4_decode(p->payload, p->len, &view);
    if (result) return result;

    netif_t *nif = net_get_default_if();
    if (view.dst != nif->ip_addr && view.dst != 0xFFFFFFFFU &&
        view.dst != (nif->ip_addr | ~nif->netmask)) return -STRATUM_EINVAL;

    p->payload += view.header_size;
    p->len = view.payload_size;
    switch (view.protocol) {
    case IP_PROTO_ICMP:
        return icmp_input(p, view.src);
    case IP_PROTO_UDP:
        return udp_input(p, view.src, view.dst);
    case IP_PROTO_TCP:
        return tcp_input(p, view.src, view.dst);
    default:
        return -STRATUM_EPROTONOSUPPORT;
    }
}

int ipv4_output(pbuf_t *p, uint32_t dst_ip, uint8_t proto) {
    if (!p) return -STRATUM_EINVAL;
    netif_t *nif = net_get_default_if();
    if (!nif || !nif->driver_priv) return -STRATUM_ENODEV;
    if (p->len > 1500 - sizeof(ip_hdr_t)) return -STRATUM_EMSGSIZE;

    size_t ip_total_len = sizeof(ip_hdr_t) + p->len;
    pbuf_t *out_p = pbuf_alloc(ip_total_len);
    if (!out_p) return -STRATUM_ENOMEM;

    ip_hdr_t *iph = (ip_hdr_t *)out_p->payload;
    iph->ver_ihl = 0x45; /* Version 4, IHL 5 (20 bytes) */
    iph->tos = 0;
    iph->total_len = htons((uint16_t)ip_total_len);
    iph->id = htons(__atomic_fetch_add(&g_ip_id_counter, 1, __ATOMIC_RELAXED));
    iph->flags_frag = htons(0x4000); /* Don't Fragment */
    iph->ttl = IP_DEFAULT_TTL;
    iph->proto = proto;
    iph->checksum = 0;
    iph->src_ip = htonl(nif->ip_addr);
    iph->dst_ip = htonl(dst_ip);
    iph->checksum = net_checksum(iph, sizeof(ip_hdr_t));

    /* Copy transport payload */
    memcpy(out_p->payload + sizeof(ip_hdr_t), p->payload, p->len);

    /* Resolve destination MAC */
    uint8_t dst_mac[ETH_ALEN];
    uint32_t next_hop = dst_ip;
    if (dst_ip != 0xFFFFFFFFU && (dst_ip & nif->netmask) != (nif->ip_addr & nif->netmask)) {
        next_hop = nif->gateway;
        if (!next_hop) {
            pbuf_free(out_p);
            return -STRATUM_ENETUNREACH;
        }
    }
    int arp_res = arp_lookup(next_hop, dst_mac);
    if (arp_res != 0) {
        arp_send_request(nif, next_hop);
        pbuf_free(out_p);
        return arp_res;
    }

    int res = ethernet_send(nif, out_p, dst_mac, ETHERTYPE_IP);
    pbuf_free(out_p);
    return res;
}
