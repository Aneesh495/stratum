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
    if (!p || p->len < sizeof(ip_hdr_t)) return -STRATUM_EINVAL;

    ip_hdr_t *iph = (ip_hdr_t *)p->payload;
    uint8_t ver = (iph->ver_ihl >> 4) & 0x0F;
    uint8_t ihl = (iph->ver_ihl & 0x0F) * 4;

    if (ver != 4 || ihl < sizeof(ip_hdr_t) || p->len < ihl) {
        return -STRATUM_EINVAL;
    }

    /* Verify checksum */
    uint16_t csum = net_checksum(iph, ihl);
    if (csum != 0) {
        return -STRATUM_EIO;
    }

    uint32_t src_ip = ntohl(iph->src_ip);
    uint32_t dst_ip = ntohl(iph->dst_ip);
    uint16_t total_len = ntohs(iph->total_len);

    netif_t *nif = net_get_default_if();
    if (total_len < ihl || p->len < total_len || !iph->ttl ||
        (ntohs(iph->flags_frag) & 0xBFFF) != 0 ||
        (dst_ip != nif->ip_addr && dst_ip != 0xFFFFFFFFU &&
         dst_ip != (nif->ip_addr | ~nif->netmask))) {
        return -STRATUM_EINVAL;
    }

    /* Advance payload beyond IP header */
    uint16_t payload_len = total_len - ihl;
    p->payload += ihl;
    p->len = payload_len;

    switch (iph->proto) {
    case IP_PROTO_ICMP:
        return icmp_input(p, src_ip);
    case IP_PROTO_UDP:
        return udp_input(p, src_ip, dst_ip);
    case IP_PROTO_TCP:
        return tcp_input(p, src_ip, dst_ip);
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
