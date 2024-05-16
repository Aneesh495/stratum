#include <stdbool.h>
#include <user/strat_api.h>
#include <shared/errno.h>
#include "ledger_store.h"

#define LEDGER_PORT 9090
#define MAX_ID (LEDGER_ID_SIZE - 1)
#define MAX_VALUE (LEDGER_VALUE_SIZE - 1)
#define REQUEST_SIZE (8 + MAX_ID + MAX_VALUE + 1)
#define CLIENT_TIMEOUT_MS 3000
#define SESSION_IDLE_MS 10000
#define CLIENT_LIMIT 32

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

static int reply(int fd, const char *text, size_t len) {
    uint64_t deadline = uptime() + CLIENT_TIMEOUT_MS;
    while (len) {
        int result = wait_fd(fd, POLLOUT, deadline);
        if (result) return result;
        int64_t n = send(fd, text, len, 0);
        if (n == -STRATUM_EAGAIN) continue;
        if (n <= 0) return n ? (int)n : -STRATUM_EIO;
        text += n;
        len -= (size_t)n;
    }
    return 0;
}

static int status_reply(int fd, const char *text) {
    return reply(fd, text, strlen(text));
}

typedef struct {
    size_t next, end;
    char bytes[512];
} stream_t;

/* 1 is a complete line, 0 is EOF between lines, and negatives are errors. */
static int receive_line(int fd, stream_t *stream, char *line, size_t cap, unsigned timeout) {
    uint64_t deadline = uptime() + timeout;
    size_t used = 0;
    for (;;) {
        if (stream->next == stream->end) {
            int ready = wait_fd(fd, POLLIN, deadline);
            if (ready) return ready;
            int64_t n = recv(fd, stream->bytes, sizeof(stream->bytes), 0);
            if (n == -STRATUM_EAGAIN) continue;
            if (n <= 0) return n < 0 ? (int)n : (used ? -STRATUM_EINVAL : 0);
            stream->next = 0;
            stream->end = (size_t)n;
        }
        char ch = stream->bytes[stream->next++];
        if (ch == '\n') {
            line[used] = 0;
            return 1;
        }
        if (ch < ' ' || ch > '~' || used + 1 == cap) return -STRATUM_EINVAL;
        line[used++] = ch;
    }
}

static void finish_reply(int fd) {
    /* Wait for the final reply bytes to be acknowledged before sending FIN. */
    if (wait_fd(fd, POLLOUT, uptime() + CLIENT_TIMEOUT_MS) == 0) shutdown(fd, SHUT_WR);
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

typedef struct {
    int queue;
    unsigned active;
    uint32_t storage_lock;
    uint32_t replica;
    ledger_store_t *store;
} service_t;

static int get_legacy(const char *path, char value[LEDGER_VALUE_SIZE]) {
    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) return fd;
    int64_t n = read(fd, value, LEDGER_VALUE_SIZE);
    close(fd);
    if (n < 0 || n > MAX_VALUE) return -STRATUM_EIO;
    if (!n) return -STRATUM_ENOENT;
    value[n] = 0;
    return 0;
}

static void lock_storage(service_t *service) {
    while (__atomic_exchange_n(&service->storage_lock, 1, __ATOMIC_ACQUIRE)) {
        strat_futex_wait(&service->storage_lock, 1, CLIENT_TIMEOUT_MS);
    }
}

static void unlock_storage(service_t *service) {
    __atomic_store_n(&service->storage_lock, 0, __ATOMIC_RELEASE);
    strat_futex_wake(&service->storage_lock, 1);
}

static int peer_append(uint32_t address, const char *id, const char *value);

static int handle_request(service_t *service, int client, char *line) {
    bool append = strncmp(line, "APPEND ", 7) == 0;
    if (!append && strncmp(line, "GET ", 4) != 0) return status_reply(client, "ERR syntax\n");

    char path[sizeof("/ledger/") + MAX_ID];
    char *id = line + (append ? 7 : 4);
    const char *rest = record_path(id, path);
    if (!rest || (append ? (*rest != ' ' || !rest[1] || strlen(rest + 1) > MAX_VALUE) : *rest != 0)) {
        return status_reply(client, "ERR syntax\n");
    }
    const char *payload = append ? rest + 1 : NULL;
    line[rest - line] = 0;

    char value[LEDGER_VALUE_SIZE];
    lock_storage(service);
    int result = ledger_store_get(service->store, id, value);
    if (result == -STRATUM_ENOENT) result = get_legacy(path, value);
    if (append) {
        if (result == 0 && strcmp(value, payload)) result = -STRATUM_EEXIST;
        if (result == -STRATUM_ENOENT) result = ledger_store_append(service->store, id, payload);
    }
    unlock_storage(service);

    if (append && result == 0 && service->replica && peer_append(service->replica, id, payload)) {
        return status_reply(client, "ERR replica\n");
    }
    if (append && result == 0) return status_reply(client, "OK durable\n");
    if (result == -STRATUM_EEXIST) return status_reply(client, "ERR conflict\n");
    if (result == -STRATUM_ENOENT) return status_reply(client, "ERR missing\n");
    if (result == -STRATUM_ENOSPC) return status_reply(client, "ERR full\n");
    if (result) return status_reply(client, "ERR storage\n");

    size_t size = strlen(value);
    char response[MAX_VALUE + 4];
    memcpy(response, "OK ", 3);
    memcpy(response + 3, value, size);
    response[3 + size] = '\n';
    return reply(client, response, size + 4);
}

int ledger_put(const char *id, const char *value) {
    char path[sizeof("/ledger/") + MAX_ID];
    const char *end = record_path(id, path);
    if (!end || *end) return -STRATUM_EINVAL;
    char prior[LEDGER_VALUE_SIZE];
    int res = ledger_store_get_disk(id, prior);
    if (res == 0) return strcmp(prior, value) ? -STRATUM_EEXIST : 0;
    if (res != -STRATUM_ENOENT) return res;
    res = prepare_storage();
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

static int connect_peer(uint32_t address) {
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) return fd;
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = port(),
        .sin_addr.s_addr = address,
    };
    int result = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
    if (result == -STRATUM_EAGAIN) result = wait_fd(fd, POLLOUT, uptime() + CLIENT_TIMEOUT_MS);
    if (!result) return fd;
    close(fd);
    return result;
}

static int peer_append(uint32_t address, const char *id, const char *value) {
    int fd = connect_peer(address);
    if (fd < 0) return fd;

    size_t id_len = strlen(id), value_len = strlen(value);
    char request[REQUEST_SIZE];
    memcpy(request, "APPEND ", 7);
    memcpy(request + 7, id, id_len);
    request[7 + id_len] = ' ';
    memcpy(request + 8 + id_len, value, value_len);
    request[8 + id_len + value_len] = '\n';

    int result = reply(fd, request, 9 + id_len + value_len);
    if (!result) {
        finish_reply(fd);
        stream_t stream = {0};
        char answer[32];
        int received = receive_line(fd, &stream, answer, sizeof(answer), CLIENT_TIMEOUT_MS);
        if (received != 1 || strcmp(answer, "OK durable")) result = -STRATUM_EIO;
    }
    close(fd);
    return result;
}

int ledger_pull(const char *peer, const char *id) {
    char path[sizeof("/ledger/") + MAX_ID];
    const char *end = record_path(id, path);
    uint32_t address = parse_ip(peer);
    if (!address || !end || *end) return -STRATUM_EINVAL;

    int fd = connect_peer(address);
    if (fd < 0) return fd;

    char request[5 + MAX_ID];
    size_t len = (size_t)(end - id);
    memcpy(request, "GET ", 4);
    memcpy(request + 4, id, len);
    request[4 + len] = '\n';
    int result = reply(fd, request, len + 5);
    char value[MAX_VALUE + 4];
    if (!result) {
        stream_t stream = {0};
        int received = receive_line(fd, &stream, value, sizeof(value), CLIENT_TIMEOUT_MS);
        if (received != 1) result = received ? received : -STRATUM_EIO;
        else if (!strcmp(value, "ERR missing")) result = -STRATUM_ENOENT;
        else if (strncmp(value, "OK ", 3)) result = -STRATUM_EIO;
    }
    if (!result) finish_reply(fd);
    close(fd);
    return result ? result : ledger_put(id, value + 3);
}

static void serve_connection(service_t *service, int client) {
    char line[REQUEST_SIZE];
    stream_t stream = {0};
    int received = receive_line(client, &stream, line, sizeof(line), CLIENT_TIMEOUT_MS);
    if (received == 1 && !strcmp(line, "SESSION 1")) {
        int result = status_reply(client, "OK session 1\n");
        while (!result) {
            received = receive_line(client, &stream, line, sizeof(line), SESSION_IDLE_MS);
            if (received != 1) break;
            result = handle_request(service, client, line);
        }
    } else if (received == 1) {
        if (stream.next == stream.end) handle_request(service, client, line);
        else status_reply(client, "ERR syntax\n");
    }
    if (received == -STRATUM_EINVAL) status_reply(client, "ERR syntax\n");
    finish_reply(client);
}

static int service_worker(void *opaque) {
    service_t *service = opaque;
    int client;
    int64_t received;
    while ((received = read(service->queue, &client, sizeof(client))) == sizeof(client)) {
        serve_connection(service, client);
        close(client);
        __atomic_sub_fetch(&service->active, 1, __ATOMIC_RELEASE);
    }
    return received == 0 ? 0 : -STRATUM_EIO;
}

int ledgerd_run(const char *peer) {
    service_t service = {0};
    if (peer) {
        service.replica = parse_ip(peer);
        if (!service.replica) return -STRATUM_EINVAL;
    }
    int result = prepare_storage();
    if (result) return result;

    int listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener < 0) return listener;
    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = port(),
    };
    result = bind(listener, (struct sockaddr *)&address, sizeof(address));
    if (!result) result = listen(listener, CLIENT_LIMIT);
    if (!result) result = ledger_store_open(&service.store);
    int work[2];
    if (!result) result = pipe(work);
    if (result) {
        if (service.store) ledger_store_close(service.store);
        close(listener);
        return result;
    }
    service.queue = work[0];

    uint32_t workers[CLIENT_LIMIT];
    unsigned started = 0;
    for (; started < CLIENT_LIMIT; started++) {
        int64_t tid = strat_thread_create(service_worker, &service, NULL);
        if (tid < 0) {
            result = (int)tid;
            break;
        }
        workers[started] = (uint32_t)tid;
    }
    if (!result) u_printf("ledgerd listening on port %d. Press q to stop.\n", LEDGER_PORT);
    while (result == 0) {
        struct pollfd fds[] = {{ .fd = listener, .events = POLLIN }, { .fd = 0, .events = POLLIN }};
        int ready = poll(fds, 2, -1);
        if (ready < 0) {
            result = ready;
            break;
        }
        if (fds[1].revents & POLLIN) {
            char ch;
            int64_t n = read(0, &ch, 1);
            if (n == 0 || (n == 1 && ch == 'q')) break;
        }
        if (!(fds[0].revents & POLLIN)) continue;
        int client = accept(listener, NULL, NULL);
        if (client < 0) continue;
        if (__atomic_load_n(&service.active, __ATOMIC_ACQUIRE) >= CLIENT_LIMIT) {
            status_reply(client, "ERR busy\n");
            finish_reply(client);
            close(client);
            continue;
        }
        __atomic_add_fetch(&service.active, 1, __ATOMIC_RELAXED);
        if (write(work[1], &client, sizeof(client)) != sizeof(client)) {
            __atomic_sub_fetch(&service.active, 1, __ATOMIC_RELAXED);
            close(client);
            result = -STRATUM_EIO;
        }
    }
    close(listener);
    close(work[1]);
    for (unsigned i = 0; i < started; i++) {
        int status = 0;
        int joined = strat_thread_join(workers[i], &status);
        if (!result) result = joined ? joined : status;
    }
    close(work[0]);
    int closed = ledger_store_close(service.store);
    return result ? result : closed;
}
