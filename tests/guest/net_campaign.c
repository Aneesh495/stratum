#include <stdbool.h>
#include <shared/errno.h>
#include <user/strat_api.h>

extern void u_printf(const char *, ...);

#define WIRE_PORT 40960
#define NETWORK_LIMIT 150000

static int address(int fd, bool remote) {
    struct sockaddr_in endpoint = {
        .sin_family = AF_INET,
        .sin_port = __builtin_bswap16(WIRE_PORT),
        .sin_addr.s_addr = __builtin_bswap32(remote ? 0x0a000210 : 0x0a00020f),
    };
    return remote ? connect(fd, (const struct sockaddr *)&endpoint, sizeof(endpoint))
                  : bind(fd, (const struct sockaddr *)&endpoint, sizeof(endpoint));
}

static unsigned limit(const char *value) {
    unsigned total = 0;
    for (; *value >= '0' && *value <= '9'; value++) {
        total = total * 10 + (*value - '0');
        if (total > NETWORK_LIMIT) return 0;
    }
    return *value ? 0 : total;
}

static int campaign(unsigned count, unsigned invalid) {
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0 || address(fd, false) || address(fd, true)) return 1;

    strat_queue_stats_t before[2], after[2];
    if (strat_net_queues(before)) return 2;
    uint64_t interrupts = strat_net_rx_irqs();
    u_printf("net-campaign: ready for %u datagrams\n", count);
    struct pollfd ready = { .fd = fd, .events = POLLIN };
    uint8_t payload[164];
    for (unsigned seq = 0; seq < count; seq++) {
        if (poll(&ready, 1, 5000) != 1 || ready.revents != POLLIN) return 3;
        size_t size = 48 + seq % 117;
        int64_t n = recv(fd, payload, sizeof(payload), 0);
        if (n != (int64_t)size) return 4;
        for (unsigned i = 0; i < size; i++) {
            uint8_t expected = i < 4 ? (uint8_t)(seq >> (8 * i))
                                     : (uint8_t)((seq * 31 + i * 17) ^ (seq >> 8));
            if (payload[i] != expected) {
                u_printf("net-campaign: payload mismatch seq=%u offset=%u\n", seq, i);
                return 5;
            }
        }
        if (send(fd, payload, size, 0) != (int64_t)size) return 6;
    }

    if (close(fd) || strat_net_queues(after)) return 7;
    for (unsigned i = 0; i < 2; i++) {
        const strat_queue_stats_t *a = &after[i], *b = &before[i];
        unsigned expected = i ? count : count + invalid;
        if (a->failed || a->submitted - b->submitted != expected || a->completed - b->completed != expected ||
            a->available_idx != (uint16_t)a->submitted || a->used_idx != (uint16_t)a->completed ||
            a->free != b->free || a->capacity != b->capacity) return 8;
        uint64_t available_wraps = (a->submitted >> 16) - (b->submitted >> 16);
        uint64_t used_wraps = (a->completed >> 16) - (b->completed >> 16);
        u_printf("[QUEUE] %s submitted=%u completed=%u available_wraps=%lu used_wraps=%lu free=%u capacity=%u\n",
                 i ? "tx" : "rx", expected, expected, available_wraps, used_wraps, a->free, a->capacity);
    }
    uint64_t received_irqs = strat_net_rx_irqs() - interrupts;
    u_printf("[IRQ] RX MSI-X completions=%lu\n", received_irqs);
    if (!received_irqs) return 9;
    u_printf("[PASS] UDP echoes=%u exact payloads\n", count);
    return 0;
}

int main(int argc, char **argv) {
    unsigned count = argc == 2 || argc == 3 ? limit(argv[1]) : 0;
    unsigned invalid = argc == 3 ? limit(argv[2]) : 0;
    if (!count || (argc == 3 && !invalid) || invalid > 10000 || count + invalid > NETWORK_LIMIT) return 1;
    int result = campaign(count, invalid);
    if (result) u_printf("net-campaign: failure=%d\n", result);
    return result;
}
