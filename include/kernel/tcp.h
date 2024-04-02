#ifndef STRATUM_KERNEL_TCP_H
#define STRATUM_KERNEL_TCP_H

#include <kernel/net.h>

#define TCP_FLAG_FIN        0x01
#define TCP_FLAG_SYN        0x02
#define TCP_FLAG_RST        0x04
#define TCP_FLAG_PSH        0x08
#define TCP_FLAG_ACK        0x10
#define TCP_FLAG_URG        0x20

#define TCP_HLEN            20
#define TCP_DEFAULT_MSS     1460
#define TCP_DEFAULT_WND     8192

#define TCP_RING_BUF_SIZE   16384

typedef struct tcp_hdr {
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t seq_num;
    uint32_t ack_num;
    uint16_t data_offset_flags;
    uint16_t window_size;
    uint16_t checksum;
    uint16_t urgent_ptr;
} __attribute__((packed)) tcp_hdr_t;

typedef enum tcp_state {
    TCP_STATE_CLOSED = 0,
    TCP_STATE_LISTEN,
    TCP_STATE_SYN_SENT,
    TCP_STATE_SYN_RECEIVED,
    TCP_STATE_ESTABLISHED,
    TCP_STATE_FIN_WAIT_1,
    TCP_STATE_FIN_WAIT_2,
    TCP_STATE_CLOSE_WAIT,
    TCP_STATE_CLOSING,
    TCP_STATE_LAST_ACK,
    TCP_STATE_TIME_WAIT
} tcp_state_t;

typedef struct tcp_pcb {
    uint32_t          local_ip;
    uint16_t          local_port;
    uint32_t          remote_ip;
    uint16_t          remote_port;
    tcp_state_t       state;

    uint32_t          snd_una;
    uint32_t          snd_nxt;
    uint32_t          snd_wnd;
    uint32_t          rcv_nxt;

    /* Receive stream ring buffer */
    uint8_t           rx_buf[TCP_RING_BUF_SIZE];
    uint32_t          rx_head;
    uint32_t          rx_tail;
    uint32_t          rx_count;

    /* One bounded retransmission slot per stream. */
    uint8_t           tx_data[TCP_DEFAULT_MSS];
    uint16_t          tx_len;
    uint8_t           tx_flags;
    unsigned          retries;
    uint64_t          retry_at;

    struct tcp_pcb   *listener;
    int               backlog_capacity;
    int               backlog_count;
    struct tcp_pcb   *next;
} tcp_pcb_t;

void tcp_init(void);
int  tcp_input(pbuf_t *p, uint32_t src_ip, uint32_t dst_ip);
void tcp_tick(void);

tcp_pcb_t *tcp_new(void);
int        tcp_bind(tcp_pcb_t *pcb, uint16_t port);
int        tcp_listen(tcp_pcb_t *pcb, int backlog);
tcp_pcb_t *tcp_accept(tcp_pcb_t *pcb);
int        tcp_connect(tcp_pcb_t *pcb, uint32_t remote_ip, uint16_t remote_port);
int64_t    tcp_send(tcp_pcb_t *pcb, const void *data, size_t len);
int64_t    tcp_recv(tcp_pcb_t *pcb, void *buf, size_t len);
int        tcp_poll(tcp_pcb_t *pcb, uint32_t events);
int        tcp_shutdown(tcp_pcb_t *pcb);
int        tcp_close(tcp_pcb_t *pcb);

#endif /* STRATUM_KERNEL_TCP_H */
