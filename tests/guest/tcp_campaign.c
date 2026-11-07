#include <shared/errno.h>
#include <user/strat_api.h>

extern int strcmp(const char *, const char *);
extern void u_printf(const char *, ...);

#define TCP_PORT 9090U
#define WORKERS 16U
#define MAX_SESSIONS 1000U
#define MAX_BYTES (1024U * 1024U)
#define BUFFER 1400U
#define WAIT_MS 15000U

typedef struct {
    uint32_t identity;
    uint32_t length;
} transfer_t;

static struct {
    int listener;
    unsigned sessions;
    unsigned claimed;
    unsigned checked;
    uint64_t bytes;
} server;

static int wait_for(int fd, int events, uint64_t deadline) {
    uint64_t now = uptime();
    if (now >= deadline) return -STRATUM_ETIMEDOUT;
    struct pollfd state = { .fd = fd, .events = events };
    int count = poll(&state, 1, (int)(deadline - now));
    return count == 1 && (state.revents & events) ? 0 : -STRATUM_ETIMEDOUT;
}

static int exact_io(int fd, void *buffer, size_t size, int events) {
    uint8_t *data = buffer;
    uint64_t deadline = uptime() + WAIT_MS;
    while (size) {
        if (wait_for(fd, events, deadline)) return -STRATUM_ETIMEDOUT;
        int64_t n = events == POLLIN ? recv(fd, data, size, 0) : send(fd, data, size, 0);
        if (n == -STRATUM_EAGAIN) continue;
        if (n <= 0) return n ? (int)n : -STRATUM_EIO;
        data += n;
        size -= (size_t)n;
    }
    return 0;
}

static uint8_t pattern(uint32_t seed, uint32_t position) {
    return (uint8_t)((seed * 31 + position * 17) ^ (position >> 8));
}

static int body(int fd, const transfer_t *transfer, int events) {
    uint8_t buffer[BUFFER];
    for (uint32_t offset = 0; offset < transfer->length;) {
        uint32_t count = transfer->length - offset;
        if (count > sizeof(buffer)) count = sizeof(buffer);
        if (events == POLLOUT) {
            for (uint32_t i = 0; i < count; i++) buffer[i] = pattern(transfer->identity, offset + i);
        }
        int result = exact_io(fd, buffer, count, events);
        if (result) return result;
        if (events == POLLIN) {
            for (uint32_t i = 0; i < count; i++) {
                if (buffer[i] != pattern(transfer->identity, offset + i)) return -STRATUM_EIO;
            }
        }
        offset += count;
    }
    return 0;
}

static int wait_eof(int fd) {
    uint8_t extra;
    uint64_t deadline = uptime() + WAIT_MS;
    for (;;) {
        if (wait_for(fd, POLLIN, deadline)) return -STRATUM_ETIMEDOUT;
        int64_t n = recv(fd, &extra, sizeof(extra), 0);
        if (n == -STRATUM_EAGAIN) continue;
        return n == 0 ? 0 : -STRATUM_EIO;
    }
}

static int finish(int fd) {
    int result = wait_for(fd, POLLOUT, uptime() + WAIT_MS);
    return result ? result : shutdown(fd, SHUT_WR);
}

static int next_client(void) {
    uint64_t deadline = uptime() + WAIT_MS;
    for (;;) {
        if (wait_for(server.listener, POLLIN, deadline)) return -STRATUM_ETIMEDOUT;
        int fd = accept(server.listener, NULL, NULL);
        if (fd != -STRATUM_EAGAIN) return fd;
    }
}

static int serve_client(int fd, transfer_t *transfer) {
    int result = exact_io(fd, transfer, sizeof(*transfer), POLLIN);
    if (result) return result;
    if (!transfer->length || transfer->length > MAX_BYTES) return -STRATUM_EINVAL;
    result = body(fd, transfer, POLLIN);
    if (!result) result = wait_eof(fd);
    if (!result) result = body(fd, transfer, POLLOUT);
    if (!result) result = finish(fd);
    return result;
}

static int serve_worker(void *unused) {
    (void)unused;
    while (__atomic_fetch_add(&server.claimed, 1, __ATOMIC_RELAXED) < server.sessions) {
        int fd = next_client();
        if (fd < 0) return fd;
        transfer_t transfer = {0};
        int result = serve_client(fd, &transfer);
        close(fd);
        if (result) {
            u_printf("tcp-campaign: server transfer=%u result=%d\n", transfer.identity, result);
            return result;
        }
        __atomic_add_fetch(&server.bytes, (uint64_t)transfer.length * 2, __ATOMIC_RELAXED);
        __atomic_add_fetch(&server.checked, 1, __ATOMIC_RELAXED);
    }
    return 0;
}

static int serve(unsigned count) {
    struct sockaddr_in address = { .sin_family = AF_INET, .sin_port = __builtin_bswap16(TCP_PORT) };
    server.listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    server.sessions = count;
    uint64_t before = strat_tcp_recovery();
    if (server.listener < 0 ||
        bind(server.listener, (const struct sockaddr *)&address, sizeof(address)) ||
        listen(server.listener, 32)) return 1;
    uint32_t workers[WORKERS];
    unsigned started = 0;
    for (; started < WORKERS && started < count; started++) {
        int64_t tid = strat_thread_create(serve_worker, NULL, NULL);
        if (tid < 0) break;
        workers[started] = (uint32_t)tid;
    }
    if (!started) return 1;
    u_printf("tcp-campaign: ready for %u sessions\n", count);
    int failed = 0;
    for (unsigned i = 0; i < started; i++) {
        int status = 0;
        if (strat_thread_join(workers[i], &status) || status) failed = 1;
    }
    close(server.listener);
    if (failed || server.checked != count) return 1;
    u_printf("[PASS] TCP server sessions=%u bytes=%lu fast_retransmits=%lu\n",
             server.checked, server.bytes, strat_tcp_recovery() - before);
    return 0;
}

static int dial(void) {
    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = __builtin_bswap16(TCP_PORT),
        .sin_addr.s_addr = __builtin_bswap32(0x0a00020f),
    };
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) return fd;
    int result = connect(fd, (const struct sockaddr *)&address, sizeof(address));
    if (result == -STRATUM_EAGAIN) result = wait_for(fd, POLLOUT, uptime() + WAIT_MS);
    if (!result) return fd;
    close(fd);
    return result;
}

static int client(unsigned sessions, unsigned length) {
    for (unsigned i = 0; i < sessions; i++) {
        int fd = dial();
        if (fd < 0) return 1;
        transfer_t transfer = { .identity = i, .length = length };
        int result = exact_io(fd, &transfer, sizeof(transfer), POLLOUT);
        if (!result) result = body(fd, &transfer, POLLOUT);
        if (!result) result = finish(fd);
        if (!result) result = body(fd, &transfer, POLLIN);
        if (!result) result = wait_eof(fd);
        close(fd);
        if (result) {
            u_printf("tcp-campaign: client transfer=%u result=%d\n", i, result);
            return 1;
        }
        if ((i + 1) % 50 == 0) u_printf("tcp-campaign: completed=%u\n", i + 1);
    }
    u_printf("[PASS] TCP client sessions=%u bytes=%lu\n", sessions, (uint64_t)sessions * length * 2);
    return 0;
}

static unsigned decimal(const char *text, unsigned maximum) {
    unsigned value = 0;
    for (; *text >= '0' && *text <= '9'; text++) {
        value = value * 10 + (unsigned)(*text - '0');
        if (value > maximum) return 0;
    }
    return *text ? 0 : value;
}

int main(int argc, char **argv) {
    unsigned count = argc >= 3 ? decimal(argv[2], MAX_SESSIONS) : 0;
    if (!count) return 1;
    if (argc == 3 && !strcmp(argv[1], "serve")) return serve(count);
    if (argc == 4 && !strcmp(argv[1], "send")) {
        unsigned length = decimal(argv[3], MAX_BYTES);
        if (length) return client(count, length);
    }
    return 1;
}
