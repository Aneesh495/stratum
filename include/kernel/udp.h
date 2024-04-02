#ifndef STRATUM_KERNEL_UDP_H
#define STRATUM_KERNEL_UDP_H

#include <kernel/net.h>

typedef struct udp_hdr {
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t length;
    uint16_t checksum;
} __attribute__((packed)) udp_hdr_t;

#define UDP_MAX_PAYLOAD 1472

typedef struct udp_pcb {
    uint16_t        local_port;
    uint16_t        remote_port;
    uint32_t        remote_ip;
    pbuf_t         *rx_head;
    pbuf_t         *rx_tail;
    unsigned        rx_count;
    struct udp_pcb *next;
} udp_pcb_t;

void udp_init(void);
int  udp_input(pbuf_t *p, uint32_t src_ip, uint32_t dst_ip);
int  udp_output(uint32_t src_ip, uint16_t src_port, uint32_t dst_ip, uint16_t dst_port,
                const void *data, size_t len);

udp_pcb_t *udp_new(void);
int        udp_bind(udp_pcb_t *pcb, uint16_t port);
int        udp_connect(udp_pcb_t *pcb, uint32_t ip, uint16_t port);
int64_t    udp_send(udp_pcb_t *pcb, const void *buf, size_t len, uint32_t ip, uint16_t port);
int64_t    udp_recv(udp_pcb_t *pcb, void *buf, size_t len);
int        udp_poll(udp_pcb_t *pcb, uint32_t events);
void       udp_close(udp_pcb_t *pcb);

#endif /* STRATUM_KERNEL_UDP_H */
