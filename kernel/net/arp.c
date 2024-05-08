#include <kernel/spinlock.h>
#include <kernel/arp.h>
#include <kernel/ethernet.h>
#include <kernel/sched.h>
#include <kernel/string.h>
#include <kernel/kernel.h>
#include <shared/errno.h>

static arp_entry_t g_arp_table[ARP_TABLE_SIZE];
static spinlock_t  g_arp_lock;

void arp_init(void) {
    spin_lock_init(&g_arp_lock);
    for (int i = 0; i < ARP_TABLE_SIZE; i++) {
        g_arp_table[i].valid = false;
    }

}

int arp_insert(uint32_t ip, const uint8_t *mac) {
    if (!mac) return -STRATUM_EINVAL;

    uint64_t flags;
    spin_lock_irqsave(&g_arp_lock, &flags);

    /* Look for existing entry */
    int free_slot = -1;
    for (int i = 0; i < ARP_TABLE_SIZE; i++) {
        if (g_arp_table[i].valid && g_arp_table[i].ip == ip) {
            memcpy(g_arp_table[i].mac, mac, ETH_ALEN);
            g_arp_table[i].timestamp_ms = timer_get_uptime_ms();
            spin_unlock_irqrestore(&g_arp_lock, flags);
            return 0;
        }
        if (!g_arp_table[i].valid && free_slot == -1) {
            free_slot = i;
        }
    }

    if (free_slot != -1) {
        g_arp_table[free_slot].ip = ip;
        memcpy(g_arp_table[free_slot].mac, mac, ETH_ALEN);
        g_arp_table[free_slot].timestamp_ms = timer_get_uptime_ms();
        g_arp_table[free_slot].valid = true;
    }

    spin_unlock_irqrestore(&g_arp_lock, flags);
    return (free_slot != -1) ? 0 : -STRATUM_ENOMEM;
}

int arp_lookup(uint32_t ip, uint8_t *out_mac) {
    if (!out_mac) return -STRATUM_EINVAL;

    /* Broadcast address */
    if (ip == 0xFFFFFFFFU) {
        memset(out_mac, 0xFF, ETH_ALEN);
        return 0;
    }

    uint64_t flags;
    spin_lock_irqsave(&g_arp_lock, &flags);

    for (int i = 0; i < ARP_TABLE_SIZE; i++) {
        if (g_arp_table[i].valid && g_arp_table[i].ip == ip) {
            memcpy(out_mac, g_arp_table[i].mac, ETH_ALEN);
            spin_unlock_irqrestore(&g_arp_lock, flags);
            return 0;
        }
    }

    spin_unlock_irqrestore(&g_arp_lock, flags);

    return -STRATUM_ENOENT;
}

int arp_send_reply(netif_t *nif, uint32_t target_ip, const uint8_t *target_mac) {
    if (!nif || !target_mac) return -STRATUM_EINVAL;

    pbuf_t *p = pbuf_alloc(sizeof(arp_hdr_t));
    if (!p) return -STRATUM_ENOMEM;

    arp_hdr_t *arp = (arp_hdr_t *)p->payload;
    arp->hw_type = htons(ARP_HW_ETHERNET);
    arp->proto_type = htons(ARP_PROTO_IP);
    arp->hw_len = ETH_ALEN;
    arp->proto_len = 4;
    arp->opcode = htons(ARP_OP_REPLY);
    memcpy(arp->sender_mac, nif->mac, ETH_ALEN);
    arp->sender_ip = htonl(nif->ip_addr);
    memcpy(arp->target_mac, target_mac, ETH_ALEN);
    arp->target_ip = htonl(target_ip);

    int res = ethernet_send(nif, p, target_mac, ETHERTYPE_ARP);
    pbuf_free(p);
    return res;
}

int arp_send_request(netif_t *nif, uint32_t target_ip) {
    if (!nif) return -STRATUM_EINVAL;

    pbuf_t *p = pbuf_alloc(sizeof(arp_hdr_t));
    if (!p) return -STRATUM_ENOMEM;

    arp_hdr_t *arp = (arp_hdr_t *)p->payload;
    arp->hw_type = htons(ARP_HW_ETHERNET);
    arp->proto_type = htons(ARP_PROTO_IP);
    arp->hw_len = ETH_ALEN;
    arp->proto_len = 4;
    arp->opcode = htons(ARP_OP_REQUEST);
    memcpy(arp->sender_mac, nif->mac, ETH_ALEN);
    arp->sender_ip = htonl(nif->ip_addr);
    memset(arp->target_mac, 0, ETH_ALEN);
    arp->target_ip = htonl(target_ip);

    uint8_t bcast[ETH_ALEN] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    int res = ethernet_send(nif, p, bcast, ETHERTYPE_ARP);
    pbuf_free(p);
    return res;
}

int arp_input(pbuf_t *p) {
    if (!p || p->len < sizeof(arp_hdr_t)) return -STRATUM_EINVAL;

    arp_hdr_t *arp = (arp_hdr_t *)p->payload;
    if (ntohs(arp->hw_type) != ARP_HW_ETHERNET || ntohs(arp->proto_type) != ARP_PROTO_IP) {
        return -STRATUM_EPROTONOSUPPORT;
    }

    uint32_t sender_ip = ntohl(arp->sender_ip);
    uint32_t target_ip = ntohl(arp->target_ip);
    uint16_t opcode = ntohs(arp->opcode);

    netif_t *nif = net_get_default_if();
    if (!nif) return -STRATUM_ENODEV;

    if (arp->hw_len != ETH_ALEN || arp->proto_len != 4 ||
        (opcode != ARP_OP_REQUEST && opcode != ARP_OP_REPLY)) return -STRATUM_EINVAL;
    if (target_ip != nif->ip_addr) return 0;
    if (opcode == ARP_OP_REPLY && memcmp(arp->target_mac, nif->mac, ETH_ALEN)) return -STRATUM_EINVAL;
    if (sender_ip && !(arp->sender_mac[0] & 1)) arp_insert(sender_ip, arp->sender_mac);
    if (opcode == ARP_OP_REQUEST) {
        /* Answer request */
        return arp_send_reply(nif, sender_ip, arp->sender_mac);
    }

    return 0;
}
