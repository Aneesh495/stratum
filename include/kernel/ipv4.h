#ifndef STRATUM_KERNEL_IPV4_H
#define STRATUM_KERNEL_IPV4_H

#include <kernel/net.h>

#define IP_PROTO_ICMP       1
#define IP_PROTO_TCP        6
#define IP_PROTO_UDP        17

#define IP_DEFAULT_TTL      64

typedef struct ip_hdr {
    uint8_t  ver_ihl;       /* Version (4 bits) + Internet Header Length (4 bits) */
    uint8_t  tos;           /* Type of Service */
    uint16_t total_len;     /* Total length in octets */
    uint16_t id;            /* Identification */
    uint16_t flags_frag;    /* Flags (3 bits) + Fragment Offset (13 bits) */
    uint8_t  ttl;           /* Time to Live */
    uint8_t  proto;         /* Protocol (ICMP, TCP, UDP) */
    uint16_t checksum;      /* Header Checksum */
    uint32_t src_ip;        /* Source IPv4 Address */
    uint32_t dst_ip;        /* Destination IPv4 Address */
} __attribute__((packed)) ip_hdr_t;

typedef struct {
    uint32_t src, dst;
    uint16_t header_size, payload_size;
    uint8_t protocol;
} ipv4_view_t;

int ipv4_decode(const void *bytes, size_t length, ipv4_view_t *out);

void ipv4_init(void);
int  ipv4_input(pbuf_t *p);
int  ipv4_output(pbuf_t *p, uint32_t dst_ip, uint8_t proto);

#endif /* STRATUM_KERNEL_IPV4_H */
