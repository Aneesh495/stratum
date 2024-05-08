#include <kernel/spinlock.h>
#include <kernel/event.h>
#include <kernel/tcp.h>
#include <kernel/ipv4.h>
#include <kernel/sched.h>
#include <kernel/slab.h>
#include <kernel/string.h>
#include <shared/errno.h>
#include <shared/syscall_nums.h>

#define TCP_PCB_LIMIT 64
#define TCP_BACKLOG_LIMIT 8
#define TCP_RETRY_MS 250
#define TCP_RETRY_LIMIT 6

static tcp_pcb_t *connections;
static spinlock_t tcp_lock = SPINLOCK_INIT;
static unsigned connection_count;
static uint16_t next_port = 49152;
static uint32_t next_iss = 100000;

static bool before(uint32_t a, uint32_t b) {
    return (int32_t)(a - b) < 0;
}

static tcp_pcb_t *allocate_pcb(void) {
    if (connection_count >= TCP_PCB_LIMIT) return NULL;
    tcp_pcb_t *pcb = kmalloc(sizeof(*pcb));
    if (!pcb) return NULL;
    memset(pcb, 0, sizeof(*pcb));
    pcb->next = connections;
    connections = pcb;
    connection_count++;
    return pcb;
}

static void remove_pcb(tcp_pcb_t *pcb) {
    tcp_pcb_t **slot = &connections;
    while (*slot != pcb) slot = &(*slot)->next;
    *slot = pcb->next;
    connection_count--;
    kfree(pcb);
}

static bool port_available(const tcp_pcb_t *pcb, uint16_t port) {
    for (const tcp_pcb_t *other = connections; other; other = other->next) {
        if (other != pcb && other->local_port == port) return false;
    }
    return true;
}

static int bind_port(tcp_pcb_t *pcb, uint16_t port) {
    if (pcb->state != TCP_STATE_CLOSED || pcb->local_port) return -STRATUM_EINVAL;
    if (!port) {
        for (unsigned i = 0; i < 16384; i++) {
            uint16_t candidate = next_port++;
            if (next_port == 0) next_port = 49152;
            if (port_available(pcb, candidate)) {
                port = candidate;
                break;
            }
        }
        if (!port) return -STRATUM_EADDRINUSE;
    }
    if (!port_available(pcb, port)) return -STRATUM_EADDRINUSE;
    pcb->local_port = port;
    return 0;
}

static int output(tcp_pcb_t *pcb, uint32_t seq, uint8_t flags, const void *data, size_t len) {
    pbuf_t *p = pbuf_alloc(sizeof(tcp_hdr_t) + len);
    if (!p) return -STRATUM_ENOMEM;
    tcp_hdr_t *header = (tcp_hdr_t *)p->payload;
    header->src_port = htons(pcb->local_port);
    header->dst_port = htons(pcb->remote_port);
    header->seq_num = htonl(seq);
    header->ack_num = htonl(pcb->rcv_nxt);
    header->data_offset_flags = htons((5U << 12) | flags);
    header->window_size = htons(TCP_RING_BUF_SIZE - pcb->rx_count);
    header->checksum = 0;
    header->urgent_ptr = 0;
    if (len) memcpy(p->payload + sizeof(*header), data, len);
    header->checksum = net_pseudo_checksum(pcb->local_ip, pcb->remote_ip, IP_PROTO_TCP, p->payload, p->len);
    int res = ipv4_output(p, pcb->remote_ip, IP_PROTO_TCP);
    pbuf_free(p);
    return res;
}

static int send_tracked(tcp_pcb_t *pcb, uint8_t flags, const void *data, size_t len) {
    int res = output(pcb, pcb->snd_nxt, flags, data, len);
    /* ARP initiated resolution. Retry the SYN when the next-hop address is known. */
    if (res != 0 && !(res == -STRATUM_ENOENT && (flags & TCP_FLAG_SYN))) return res;
    if (len) memcpy(pcb->tx_data, data, len);
    pcb->tx_flags = flags;
    pcb->tx_len = (uint16_t)len;
    pcb->snd_nxt += (uint32_t)len + ((flags & (TCP_FLAG_SYN | TCP_FLAG_FIN)) ? 1U : 0U);
    pcb->retry_at = timer_get_uptime_ms() + TCP_RETRY_MS;
    pcb->retries = 0;
    return 0;
}

static void acknowledge(tcp_pcb_t *pcb, uint32_t ack, uint16_t window) {
    if (before(ack, pcb->snd_una) || before(pcb->snd_nxt, ack)) return;
    pcb->snd_wnd = window;
    /* Track one complete segment. A partial ACK leaves it eligible for retry. */
    if (ack == pcb->snd_nxt) pcb->snd_una = ack;
}

void tcp_init(void) {
    spin_lock_init(&tcp_lock);
}

tcp_pcb_t *tcp_new(void) {
    uint64_t flags;
    spin_lock_irqsave(&tcp_lock, &flags);
    tcp_pcb_t *pcb = allocate_pcb();
    spin_unlock_irqrestore(&tcp_lock, flags);
    return pcb;
}

int tcp_bind(tcp_pcb_t *pcb, uint16_t port) {
    uint64_t flags;
    spin_lock_irqsave(&tcp_lock, &flags);
    int res = bind_port(pcb, port);
    spin_unlock_irqrestore(&tcp_lock, flags);
    return res;
}

int tcp_listen(tcp_pcb_t *pcb, int backlog) {
    uint64_t flags;
    spin_lock_irqsave(&tcp_lock, &flags);
    int res = 0;
    if (pcb->state != TCP_STATE_CLOSED) res = -STRATUM_EINVAL;
    else if (!pcb->local_port) res = bind_port(pcb, 0);
    if (res == 0) {
        pcb->state = TCP_STATE_LISTEN;
        pcb->backlog_capacity = backlog < 1 ? 1 : (backlog > TCP_BACKLOG_LIMIT ? TCP_BACKLOG_LIMIT : backlog);
    }
    spin_unlock_irqrestore(&tcp_lock, flags);
    return res;
}

static void start_connection(tcp_pcb_t *pcb, uint32_t remote_ip, uint16_t remote_port) {
    pcb->local_ip = net_get_default_if()->ip_addr;
    pcb->remote_ip = remote_ip;
    pcb->remote_port = remote_port;
    pcb->snd_nxt = next_iss += 4099;
    pcb->snd_una = pcb->snd_nxt;
}

int tcp_connect(tcp_pcb_t *pcb, uint32_t remote_ip, uint16_t remote_port) {
    if (!remote_ip || !remote_port) return -STRATUM_EINVAL;
    uint64_t flags;
    spin_lock_irqsave(&tcp_lock, &flags);
    int res = 0;
    if (pcb->state == TCP_STATE_ESTABLISHED) res = -STRATUM_EISCONN;
    else if (pcb->state != TCP_STATE_CLOSED) res = -STRATUM_EAGAIN;
    else {
        if (!pcb->local_port) res = bind_port(pcb, 0);
        if (res == 0) {
            start_connection(pcb, remote_ip, remote_port);
            res = send_tracked(pcb, TCP_FLAG_SYN, NULL, 0);
            if (res == 0) {
                pcb->state = TCP_STATE_SYN_SENT;
                res = -STRATUM_EAGAIN;
            }
        }
    }
    spin_unlock_irqrestore(&tcp_lock, flags);
    return res;
}

tcp_pcb_t *tcp_accept(tcp_pcb_t *listener) {
    uint64_t flags;
    spin_lock_irqsave(&tcp_lock, &flags);
    tcp_pcb_t *accepted = NULL;
    if (listener->state == TCP_STATE_LISTEN) {
        for (tcp_pcb_t *child = connections; child; child = child->next) {
            if (child->listener == listener &&
                (child->state == TCP_STATE_ESTABLISHED || child->state == TCP_STATE_CLOSE_WAIT)) {
                child->listener = NULL;
                listener->backlog_count--;
                accepted = child;
                break;
            }
        }
    }
    spin_unlock_irqrestore(&tcp_lock, flags);
    return accepted;
}

int64_t tcp_send(tcp_pcb_t *pcb, const void *data, size_t len) {
    uint64_t flags;
    spin_lock_irqsave(&tcp_lock, &flags);
    int64_t res = -STRATUM_ENOTCONN;
    if (pcb->state == TCP_STATE_ESTABLISHED || pcb->state == TCP_STATE_CLOSE_WAIT) {
        if (!len) res = 0;
        else if (pcb->snd_una != pcb->snd_nxt || pcb->snd_wnd == 0) res = -STRATUM_EAGAIN;
        else {
            size_t n = len < TCP_DEFAULT_MSS ? len : TCP_DEFAULT_MSS;
            if (n > pcb->snd_wnd) n = pcb->snd_wnd;
            int sent = send_tracked(pcb, TCP_FLAG_ACK | TCP_FLAG_PSH, data, n);
            res = sent ? sent : (int64_t)n;
        }
    }
    spin_unlock_irqrestore(&tcp_lock, flags);
    return res;
}

static bool receive_eof(const tcp_pcb_t *pcb) {
    return pcb->state == TCP_STATE_CLOSE_WAIT || pcb->state == TCP_STATE_LAST_ACK ||
        pcb->state == TCP_STATE_CLOSING || pcb->state == TCP_STATE_TIME_WAIT ||
        pcb->state == TCP_STATE_CLOSED;
}

int64_t tcp_recv(tcp_pcb_t *pcb, void *buf, size_t len) {
    if (!len) return 0;
    uint64_t flags;
    spin_lock_irqsave(&tcp_lock, &flags);
    size_t count = len < pcb->rx_count ? len : pcb->rx_count;
    int64_t res = receive_eof(pcb) ? 0 : -STRATUM_EAGAIN;
    if (count) {
        for (size_t i = 0; i < count; i++) {
            ((uint8_t *)buf)[i] = pcb->rx_buf[pcb->rx_tail];
            pcb->rx_tail = (pcb->rx_tail + 1) % TCP_RING_BUF_SIZE;
        }
        bool reopen = pcb->rx_count == TCP_RING_BUF_SIZE;
        pcb->rx_count -= (uint32_t)count;
        if (reopen) output(pcb, pcb->snd_nxt, TCP_FLAG_ACK, NULL, 0);
        res = (int64_t)count;
    }
    spin_unlock_irqrestore(&tcp_lock, flags);
    return res;
}

int tcp_poll(tcp_pcb_t *pcb, uint32_t events) {
    uint64_t flags;
    spin_lock_irqsave(&tcp_lock, &flags);
    int res = 0;
    if (events & POLLIN) {
        if (pcb->state == TCP_STATE_LISTEN) {
            for (tcp_pcb_t *child = connections; child; child = child->next) {
                if (child->listener == pcb && (child->state == TCP_STATE_ESTABLISHED ||
                                               child->state == TCP_STATE_CLOSE_WAIT)) {
                    res |= POLLIN;
                    break;
                }
            }
        } else if (pcb->rx_count || receive_eof(pcb)) res |= POLLIN;
    }
    if ((events & POLLOUT) && (pcb->state == TCP_STATE_ESTABLISHED || pcb->state == TCP_STATE_CLOSE_WAIT) &&
        pcb->snd_una == pcb->snd_nxt && pcb->snd_wnd) res |= POLLOUT;
    if (pcb->state == TCP_STATE_CLOSED && pcb->remote_port) res |= POLLHUP;
    spin_unlock_irqrestore(&tcp_lock, flags);
    return res;
}

int tcp_shutdown(tcp_pcb_t *pcb) {
    uint64_t flags;
    spin_lock_irqsave(&tcp_lock, &flags);
    int res = -STRATUM_ENOTCONN;
    if (pcb->state == TCP_STATE_ESTABLISHED || pcb->state == TCP_STATE_CLOSE_WAIT) {
        if (pcb->snd_una != pcb->snd_nxt) res = -STRATUM_EAGAIN;
        else {
            res = send_tracked(pcb, TCP_FLAG_FIN | TCP_FLAG_ACK, NULL, 0);
            if (res == 0) pcb->state = pcb->state == TCP_STATE_ESTABLISHED ? TCP_STATE_FIN_WAIT_1 : TCP_STATE_LAST_ACK;
        }
    }
    spin_unlock_irqrestore(&tcp_lock, flags);
    return res;
}

int tcp_close(tcp_pcb_t *pcb) {
    if (!pcb) return -STRATUM_EINVAL;
    uint64_t flags;
    spin_lock_irqsave(&tcp_lock, &flags);
    if (pcb->state == TCP_STATE_LISTEN) {
        tcp_pcb_t *cur = connections;
        while (cur) {
            tcp_pcb_t *next = cur->next;
            if (cur->listener == pcb) remove_pcb(cur);
            cur = next;
        }
    }
    if (pcb->state == TCP_STATE_ESTABLISHED || pcb->state == TCP_STATE_CLOSE_WAIT) {
        output(pcb, pcb->snd_nxt, TCP_FLAG_RST | TCP_FLAG_ACK, NULL, 0);
    }
    remove_pcb(pcb);
    spin_unlock_irqrestore(&tcp_lock, flags);
    return 0;
}

static void receive_stream(tcp_pcb_t *pcb, uint32_t seq, uint8_t flags,
                           const uint8_t *data, size_t len) {
    if (seq != pcb->rcv_nxt || len > TCP_RING_BUF_SIZE - pcb->rx_count) {
        output(pcb, pcb->snd_nxt, TCP_FLAG_ACK, NULL, 0);
        return;
    }
    for (size_t i = 0; i < len; i++) {
        pcb->rx_buf[pcb->rx_head] = data[i];
        pcb->rx_head = (pcb->rx_head + 1) % TCP_RING_BUF_SIZE;
    }
    pcb->rx_count += (uint32_t)len;
    pcb->rcv_nxt += (uint32_t)len;
    if (flags & TCP_FLAG_FIN) {
        pcb->rcv_nxt++;
        if (pcb->state == TCP_STATE_ESTABLISHED) pcb->state = TCP_STATE_CLOSE_WAIT;
        else if (pcb->state == TCP_STATE_FIN_WAIT_1) pcb->state = TCP_STATE_CLOSING;
        else if (pcb->state == TCP_STATE_FIN_WAIT_2) pcb->state = TCP_STATE_TIME_WAIT;
    }
    if (len || (flags & TCP_FLAG_FIN)) output(pcb, pcb->snd_nxt, TCP_FLAG_ACK, NULL, 0);
}

int tcp_input(pbuf_t *p, uint32_t src_ip, uint32_t dst_ip) {
    if (!p || p->len < sizeof(tcp_hdr_t) || dst_ip != net_get_default_if()->ip_addr) return -STRATUM_EINVAL;
    const tcp_hdr_t *th = (const tcp_hdr_t *)p->payload;
    uint16_t word = ntohs(th->data_offset_flags);
    size_t header_len = (word >> 12) * 4;
    if (header_len < sizeof(*th) || header_len > p->len) return -STRATUM_EINVAL;
    if (net_pseudo_checksum(src_ip, dst_ip, IP_PROTO_TCP, p->payload, p->len)) return -STRATUM_EIO;

    uint16_t src = ntohs(th->src_port), dst = ntohs(th->dst_port);
    if (!src || !dst) return -STRATUM_EINVAL;
    uint32_t seq = ntohl(th->seq_num), ack = ntohl(th->ack_num);
    uint8_t packet_flags = (uint8_t)word;
    uint16_t window = ntohs(th->window_size);

    uint64_t flags;
    spin_lock_irqsave(&tcp_lock, &flags);
    tcp_pcb_t *pcb = NULL, *listener = NULL;
    for (tcp_pcb_t *cur = connections; cur; cur = cur->next) {
        if (cur->local_port != dst) continue;
        if (cur->state == TCP_STATE_LISTEN) listener = cur;
        else if (cur->state != TCP_STATE_CLOSED && cur->remote_port == src && cur->remote_ip == src_ip) {
            pcb = cur;
            break;
        }
    }
    if (!pcb) {
        if (listener && (packet_flags & (TCP_FLAG_SYN | TCP_FLAG_RST | TCP_FLAG_ACK)) == TCP_FLAG_SYN &&
            listener->backlog_count < listener->backlog_capacity) {
            pcb = allocate_pcb();
            if (pcb) {
                start_connection(pcb, src_ip, src);
                pcb->local_port = dst;
                pcb->rcv_nxt = seq + 1;
                pcb->snd_wnd = window;
                if (send_tracked(pcb, TCP_FLAG_SYN | TCP_FLAG_ACK, NULL, 0) == 0) {
                    pcb->state = TCP_STATE_SYN_RECEIVED;
                    pcb->listener = listener;
                    listener->backlog_count++;
                } else remove_pcb(pcb);
            }
        }
        spin_unlock_irqrestore(&tcp_lock, flags);
        return 0;
    }

    if (packet_flags & TCP_FLAG_RST) {
        if ((pcb->state == TCP_STATE_SYN_SENT && (packet_flags & TCP_FLAG_ACK) && ack == pcb->snd_nxt) ||
            (pcb->state != TCP_STATE_SYN_SENT && seq == pcb->rcv_nxt)) pcb->state = TCP_STATE_CLOSED;
        goto out;
    }
    if (pcb->state == TCP_STATE_SYN_SENT) {
        if ((packet_flags & (TCP_FLAG_SYN | TCP_FLAG_ACK)) == (TCP_FLAG_SYN | TCP_FLAG_ACK) &&
            ack == pcb->snd_nxt) {
            pcb->rcv_nxt = seq + 1;
            acknowledge(pcb, ack, window);
            pcb->state = TCP_STATE_ESTABLISHED;
            output(pcb, pcb->snd_nxt, TCP_FLAG_ACK, NULL, 0);
        }
        goto out;
    }
    if (packet_flags & TCP_FLAG_SYN) {
        if (seq + 1 == pcb->rcv_nxt) {
            if (pcb->state == TCP_STATE_SYN_RECEIVED) {
                output(pcb, pcb->snd_una, pcb->tx_flags, NULL, 0);
            } else output(pcb, pcb->snd_nxt, TCP_FLAG_ACK, NULL, 0);
        }
        goto out;
    }
    if (!(packet_flags & TCP_FLAG_ACK) || before(pcb->snd_nxt, ack)) goto out;
    if (pcb->state == TCP_STATE_SYN_RECEIVED) {
        if (ack != pcb->snd_nxt || seq != pcb->rcv_nxt) goto out;
        pcb->state = TCP_STATE_ESTABLISHED;
    }
    acknowledge(pcb, ack, window);
    if (pcb->snd_una == pcb->snd_nxt) {
        if (pcb->state == TCP_STATE_FIN_WAIT_1) pcb->state = TCP_STATE_FIN_WAIT_2;
        else if (pcb->state == TCP_STATE_CLOSING) pcb->state = TCP_STATE_TIME_WAIT;
        else if (pcb->state == TCP_STATE_LAST_ACK) pcb->state = TCP_STATE_CLOSED;
    }
    if (pcb->state == TCP_STATE_ESTABLISHED || pcb->state == TCP_STATE_FIN_WAIT_1 ||
        pcb->state == TCP_STATE_FIN_WAIT_2) {
        receive_stream(pcb, seq, packet_flags, p->payload + header_len, p->len - header_len);
    } else if (packet_flags & TCP_FLAG_FIN) {
        output(pcb, pcb->snd_nxt, TCP_FLAG_ACK, NULL, 0);
    }
out:
    spin_unlock_irqrestore(&tcp_lock, flags);
    return 0;
}

void tcp_tick(void) {
    uint64_t flags;
    spin_lock_irqsave(&tcp_lock, &flags);
    uint64_t now = timer_get_uptime_ms();
    bool closed = false;
    for (tcp_pcb_t *pcb = connections; pcb;) {
        tcp_pcb_t *next = pcb->next;
        if (pcb->state != TCP_STATE_CLOSED && pcb->snd_nxt != pcb->snd_una && now >= pcb->retry_at) {
            if (pcb->retries++ == TCP_RETRY_LIMIT) {
                pcb->state = TCP_STATE_CLOSED;
                closed = true;
            } else {
                output(pcb, pcb->snd_una, pcb->tx_flags, pcb->tx_data, pcb->tx_len);
                pcb->retry_at = now + ((uint64_t)TCP_RETRY_MS << (pcb->retries > 3 ? 3 : pcb->retries));
            }
        }
        if (pcb->state == TCP_STATE_CLOSED && pcb->listener) {
            pcb->listener->backlog_count--;
            remove_pcb(pcb);
        }
        pcb = next;
    }
    spin_unlock_irqrestore(&tcp_lock, flags);
    if (closed) readiness_notify();
}
