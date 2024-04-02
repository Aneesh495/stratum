#include <stdbool.h>
#include <user/strat_api.h>
#include <shared/errno.h>

#define LEDGER_PORT 9090
#define MAX_ID 24
#define MAX_VALUE 128
#define CLIENT_TIMEOUT_MS 3000

extern void u_printf(const char *fmt, ...);
extern size_t strlen(const char *s);
extern int strncmp(const char *a, const char *b, size_t n);
extern int strcmp(const char *a, const char *b);
extern int memcmp(const void *a, const void *b, size_t n);
extern void *memcpy(void *dst, const void *src, size_t n);

static int wait_fd(int fd, int events, uint64_t deadline) {
    uint64_t now = uptime();
    if (now >= deadline) return -STRATUM_ETIMEDOUT;
    struct pollfd p = { .fd = fd, .events = events };
    int res = poll(&p, 1, (int)(deadline - now));
    if (res < 0) return res;
    if (p.revents & events) return 0;
    return res == 0 ? -STRATUM_ETIMEDOUT : -STRATUM_EIO;
}

static void reply(int fd, const char *text, size_t len) {
    uint64_t deadline = uptime() + CLIENT_TIMEOUT_MS;
    while (len && wait_fd(fd, POLLOUT, deadline) == 0) {
        int64_t n = send(fd, text, len, 0);
        if (n == -STRATUM_EAGAIN) continue;
        if (n <= 0) return;
        text += n;
        len -= (size_t)n;
    }
    /* Wait for the final reply bytes to be acknowledged before sending FIN. */
    if (!len && wait_fd(fd, POLLOUT, deadline) == 0) shutdown(fd, SHUT_WR);
}

static void status_reply(int fd, const char *text) {
    reply(fd, text, strlen(text));
}

static int receive_request(int fd, char *line, size_t cap) {
    uint64_t deadline = uptime() + CLIENT_TIMEOUT_MS;
    size_t used = 0;
    while (used < cap && wait_fd(fd, POLLIN, deadline) == 0) {
        int64_t n = recv(fd, line + used, cap - used, 0);
        if (n == -STRATUM_EAGAIN) continue;
        if (n <= 0) break;
        size_t end = used + (size_t)n;
        for (; used < end; used++) {
            if (line[used] == '\n') {
                if (used + 1 != end) return -STRATUM_EINVAL;
                line[used] = '\0';
                return 0;
            }
            if (line[used] < ' ' || line[used] > '~') return -STRATUM_EINVAL;
        }
    }
    return -STRATUM_EINVAL;
}

static const char *record_path(const char *input, char path[sizeof("/ledger/") + MAX_ID]) {
    memcpy(path, "/ledger/", sizeof("/ledger/") - 1);
    size_t n = 0;
    while (input[n] && input[n] != ' ') {
        char c = input[n];
        if (n == MAX_ID || !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || c == '-')) return NULL;
        path[sizeof("/ledger/") - 1 + n] = c;
        n++;
    }
    if (!n) return NULL;
    path[sizeof("/ledger/") - 1 + n] = '\0';
    return input + n;
}

static int prepare_storage(void) {
    int res = mkdir("/ledger", 0755);
    return res == -STRATUM_EEXIST ? 0 : res;
}

static int store(const char *path, const char *value) {
    size_t len = strlen(value);
    if (!len || len > MAX_VALUE) return -STRATUM_EINVAL;
    int fd = open(path, O_RDWR | O_CREAT, 0644);
    if (fd < 0) return fd;

    char stored[MAX_VALUE + 1];
    int64_t n = read(fd, stored, sizeof(stored));
    int res;
    if (n < 0 || n > MAX_VALUE) res = -STRATUM_EIO;
    else if (n && ((size_t)n != len || memcmp(stored, value, len) != 0)) res = -STRATUM_EEXIST;
    else if (!n && write(fd, value, len) != (int64_t)len) res = -STRATUM_EIO;
    else res = fsync(fd);
    close(fd);
    return res;
}

static void handle_request(int client, char *line) {
    bool append = strncmp(line, "APPEND ", 7) == 0;
    if (!append && strncmp(line, "GET ", 4) != 0) {
        status_reply(client, "ERR syntax\n");
        return;
    }

    char path[sizeof("/ledger/") + MAX_ID];
    const char *rest = record_path(line + (append ? 7 : 4), path);
    if (!rest || (append ? (*rest != ' ' || !rest[1] || strlen(rest + 1) > MAX_VALUE) : *rest != 0)) {
        status_reply(client, "ERR syntax\n");
        return;
    }
    if (append) {
        int res = store(path, rest + 1);
        status_reply(client, res == 0 ? "OK durable\n" :
                      (res == -STRATUM_EEXIST ? "ERR conflict\n" : "ERR storage\n"));
        return;
    }

    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) {
        status_reply(client, fd == -STRATUM_ENOENT ? "ERR missing\n" : "ERR storage\n");
        return;
    }

    char stored[MAX_VALUE + 1];
    int64_t n = read(fd, stored, sizeof(stored));
    close(fd);
    if (n < 0 || n > MAX_VALUE) status_reply(client, "ERR storage\n");
    else if (!n) status_reply(client, "ERR missing\n");
    else {
        char response[MAX_VALUE + 4];
        memcpy(response, "OK ", 3);
        memcpy(response + 3, stored, (size_t)n);
        response[3 + n] = '\n';
        reply(client, response, (size_t)n + 4);
    }
}

int ledger_put(const char *id, const char *value) {
    char path[sizeof("/ledger/") + MAX_ID];
    const char *end = record_path(id, path);
    if (!end || *end) return -STRATUM_EINVAL;
    int res = prepare_storage();
    return res ? res : store(path, value);
}

static uint32_t parse_ip(const char *text) {
    uint32_t address = 0;
    for (unsigned i = 0; i < 4; i++) {
        unsigned value = 0;
        unsigned digits = 0;
        while (*text >= '0' && *text <= '9' && digits < 3) {
            value = value * 10 + (unsigned)(*text++ - '0');
            digits++;
        }
        if (!digits || value > 255 || (i < 3 ? *text++ != '.' : *text != 0)) return 0;
        address |= value << (8 * i);
    }
    return address;
}

static uint16_t port(void) {
    return (uint16_t)((LEDGER_PORT >> 8) | (LEDGER_PORT << 8));
}

int ledger_pull(const char *peer, const char *id) {
    char path[sizeof("/ledger/") + MAX_ID];
    const char *end = record_path(id, path);
    uint32_t address = parse_ip(peer);
    if (!address || !end || *end) return -STRATUM_EINVAL;

    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) return fd;
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = port(),
        .sin_addr.s_addr = address,
    };
    int res = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
    if (res == -STRATUM_EAGAIN) res = wait_fd(fd, POLLOUT, uptime() + CLIENT_TIMEOUT_MS);
    if (res == 0) {
        char request[5 + MAX_ID];
        size_t len = (size_t)(end - id);
        memcpy(request, "GET ", 4);
        memcpy(request + 4, id, len);
        request[4 + len] = '\n';
        if (send(fd, request, len + 5, 0) != (int64_t)(len + 5)) res = -STRATUM_EIO;
    }
    char value[MAX_VALUE + 4];
    if (res == 0) res = receive_request(fd, value, sizeof(value));
    if (res == 0 && strcmp(value, "ERR missing") == 0) res = -STRATUM_ENOENT;
    if (res == 0 && strncmp(value, "OK ", 3) != 0) res = -STRATUM_EIO;
    if (res == 0) shutdown(fd, SHUT_WR);
    close(fd);
    return res ? res : ledger_put(id, value + 3);
}

int ledgerd_run(void) {
    int res = prepare_storage();
    if (res != 0) return res;
    int listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener < 0) return listener;
    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = port(),
    };
    res = bind(listener, (struct sockaddr *)&address, sizeof(address));
    if (res == 0) res = listen(listener, 4);
    if (res != 0) {
        close(listener);
        return res;
    }
    u_printf("ledgerd listening on port %d. Press q to stop.\n", LEDGER_PORT);
    for (;;) {
        struct pollfd fds[] = {{ .fd = listener, .events = POLLIN }, { .fd = 0, .events = POLLIN }};
        res = poll(fds, 2, -1);
        if (res < 0) break;
        if (fds[1].revents & POLLIN) {
            char ch;
            if (read(0, &ch, 1) == 1 && ch == 'q') {
                res = 0;
                break;
            }
        }
        if (!(fds[0].revents & POLLIN)) continue;
        int client = accept(listener, NULL, NULL);
        if (client < 0) continue;
        char line[8 + MAX_ID + MAX_VALUE + 1];
        if (receive_request(client, line, sizeof(line)) == 0) handle_request(client, line);
        else status_reply(client, "ERR syntax\n");
        close(client);
    }
    close(listener);
    return res;
}
