#ifndef STRATUM_KERNEL_NET_H
#define STRATUM_KERNEL_NET_H

#include <kernel/types.h>

#define PBUF_MAX_SIZE       1600
#define ETH_ALEN            6
#define ETH_HLEN            14
#define ETH_DATA_LEN        1500
#define ETH_FRAME_LEN       1514

#define ETHERTYPE_IP        0x0800
#define ETHERTYPE_ARP       0x0806

#define IP4_ADDR(a, b, c, d) \
    (((uint32_t)((a) & 0xFF) << 24) | \
     ((uint32_t)((b) & 0xFF) << 16) | \
     ((uint32_t)((c) & 0xFF) << 8)  | \
      (uint32_t)((d) & 0xFF))

static inline uint16_t htons(uint16_t v) {
    return (uint16_t)(((v & 0x00FFU) << 8) | ((v & 0xFF00U) >> 8));
}

static inline uint16_t ntohs(uint16_t v) {
    return htons(v);
}

static inline uint32_t htonl(uint32_t v) {
    return ((v & 0x000000FFU) << 24) |
           ((v & 0x0000FF00U) << 8)  |
           ((v & 0x00FF0000U) >> 8)  |
           ((v & 0xFF000000U) >> 24);
}

static inline uint32_t ntohl(uint32_t v) {
    return htonl(v);
}

typedef struct pbuf {
    struct pbuf      *next;
    uint8_t          *payload;
    uint16_t          len;
    uint16_t          tot_len;
    volatile uint32_t refcount;
    uint8_t           data[PBUF_MAX_SIZE];
} pbuf_t;

struct netif;

typedef struct netif {
    char             name[16];
    uint8_t          mac[ETH_ALEN];
    uint32_t         ip_addr;   /* Host byte order */
    uint32_t         netmask;   /* Host byte order */
    uint32_t         gateway;   /* Host byte order */
    void            *driver_priv;
    int (*output)(struct netif *nif, pbuf_t *p, const uint8_t *dst_mac, uint16_t ethertype);
    struct netif    *next;
} netif_t;

/* Checksum calculation for IPv4 / ICMP / UDP / TCP */
uint16_t net_checksum(const void *data, size_t length);
uint16_t net_pseudo_checksum(uint32_t src_ip, uint32_t dst_ip, uint8_t proto, const void *payload, size_t length);

/* Packet buffer management */
pbuf_t *pbuf_alloc(size_t size);
void    pbuf_ref(pbuf_t *p);
void    pbuf_free(pbuf_t *p);

/* Network Subsystem API */
void     net_init(void);
void     net_start_worker(void);
netif_t *net_get_default_if(void);
void     net_input(pbuf_t *p);
void     net_poll(void);

#endif /* STRATUM_KERNEL_NET_H */
