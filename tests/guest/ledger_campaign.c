#include <shared/errno.h>
#include <user/strat_api.h>

extern int strcmp(const char *, const char *);
extern size_t strlen(const char *);
extern void *memcpy(void *, const void *, size_t);
extern void u_dprintf(int, const char *, ...);

#define CLIENTS 32U
#define MAX_KEYS 10000U
#define OPS_PER_KEY 10U
#define REQUEST_MS 3000U
#define RECOVERY_MS 45000U

static unsigned keys;
static unsigned arrived, start, completed, attempts, failed;

static int wait_for(int fd, int events, uint64_t deadline) {
    uint64_t now = uptime();
    if (now >= deadline) return -STRATUM_ETIMEDOUT;
    struct pollfd state = { .fd = fd, .events = events };
    int result = poll(&state, 1, (int)(deadline - now));
    return result == 1 && (state.revents & events) ? 0 : -STRATUM_ETIMEDOUT;
}

static int exchange(int fd, const char *request, char *response, size_t capacity) {
    uint64_t deadline = uptime() + REQUEST_MS;
    size_t remaining = strlen(request);
    while (remaining) {
        if (wait_for(fd, POLLOUT, deadline)) return -1;
        int64_t size = send(fd, request, remaining, 0);
        if (size == -STRATUM_EAGAIN) continue;
        if (size <= 0) return -1;
        request += size;
        remaining -= (size_t)size;
    }
    size_t used = 0;
    while (used + 1 < capacity) {
        if (wait_for(fd, POLLIN, deadline)) return -1;
        int64_t size = recv(fd, response + used, capacity - used - 1, 0);
        if (size == -STRATUM_EAGAIN) continue;
        if (size <= 0) return -1;
        size_t end = used + (size_t)size;
        for (; used < end; used++) {
            if (response[used] == '\n') {
                response[used] = 0;
                return used + 1 == end ? 0 : -1;
            }
        }
    }
    return -1;
}

static int start_session(void) {
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) return fd;
    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = __builtin_bswap16(9090),
        .sin_addr.s_addr = __builtin_bswap32(0x0a00020f),
    };
    int result = connect(fd, (const struct sockaddr *)&address, sizeof(address));
    if (result == -STRATUM_EAGAIN) result = wait_for(fd, POLLOUT, uptime() + REQUEST_MS);
    char answer[160];
    if (!result && !exchange(fd, "SESSION 1\n", answer, sizeof(answer)) &&
        !strcmp(answer, "OK session 1")) return fd;
    close(fd);
    return -1;
}

static void id_text(unsigned key, char id[10]) {
    memcpy(id, "entry", 5);
    for (unsigned i = 0; i < 4; i++) {
        id[8 - i] = (char)('0' + key % 10);
        key /= 10;
    }
    id[9] = 0;
}

static void value_text(unsigned key, char value[65]) {
    for (unsigned i = 0; i < 64; i++) value[i] = (char)('a' + ((key * 13) ^ (i * 29 + key / 100)) % 26);
    value[64] = 0;
}

static int request_once(int *fd, const char *request, const char *expected) {
    uint64_t deadline = uptime() + RECOVERY_MS;
    char response[160];
    while (uptime() < deadline) {
        __atomic_add_fetch(&attempts, 1, __ATOMIC_RELAXED);
        if (*fd < 0) *fd = start_session();
        if (*fd >= 0) {
            int result = exchange(*fd, request, response, sizeof(response));
            if (result == 0 && strcmp(response, "ERR replica") != 0) {
                if (!strcmp(response, expected)) return 0;
                u_dprintf(2, "ledger-campaign: expected %s, got %s\n", expected, response);
                return 1;
            }
            close(*fd);
            *fd = -1;
        }
        sleep_ms(50);
    }
    return 1;
}

static int run_requests(unsigned index, int fd) {
    for (unsigned key = index; key < keys; key += CLIENTS) {
        char id[10], value[65], append[83], get[15], expected[68];
        id_text(key, id);
        value_text(key, value);
        memcpy(append, "APPEND ", 7);
        memcpy(append + 7, id, 9);
        append[16] = ' ';
        memcpy(append + 17, value, 64);
        append[81] = '\n';
        append[82] = 0;
        memcpy(get, "GET ", 4);
        memcpy(get + 4, id, 9);
        get[13] = '\n';
        get[14] = 0;
        memcpy(expected, "OK ", 3);
        memcpy(expected + 3, value, sizeof(value));
        for (unsigned op = 0; op < OPS_PER_KEY; op++) {
            if (op == 2) append[17] = value[0] == 'z' ? 'a' : value[0] + 1;
            const char *reply = op < 2 ? "OK durable" : (op == 2 ? "ERR conflict" : expected);
            if (request_once(&fd, op < 3 ? append : get, reply)) {
                if (fd >= 0) close(fd);
                return 1;
            }
            if (op == 0) u_dprintf(2, "[RECORD] %s durable\n", id);
            unsigned done = __atomic_add_fetch(&completed, 1, __ATOMIC_RELAXED);
            if (done % 1000 == 0) u_dprintf(2, "[LEDGER] requests=%u\n", done);
        }
    }
    if (fd >= 0) {
        if (!wait_for(fd, POLLOUT, uptime() + REQUEST_MS)) shutdown(fd, SHUT_WR);
        close(fd);
    }
    return 0;
}

static int worker(void *opaque) {
    unsigned index = (unsigned)(uintptr_t)opaque;
    uint64_t deadline = uptime() + RECOVERY_MS;
    int fd = -1;
    while (fd < 0 && uptime() < deadline) {
        fd = start_session();
        if (fd < 0) sleep_ms(50);
    }
    if (fd < 0) {
        __atomic_store_n(&failed, 1, __ATOMIC_RELEASE);
        strat_futex_wake(&start, CLIENTS);
        return 1;
    }
    if (__atomic_add_fetch(&arrived, 1, __ATOMIC_ACQ_REL) == CLIENTS) {
        u_dprintf(2, "[LEDGER] clients=%u live sessions\n", CLIENTS);
        __atomic_store_n(&start, 1, __ATOMIC_RELEASE);
        strat_futex_wake(&start, CLIENTS);
    }
    while (!__atomic_load_n(&start, __ATOMIC_ACQUIRE) && !__atomic_load_n(&failed, __ATOMIC_ACQUIRE)) {
        if (uptime() >= deadline) {
            close(fd);
            return 1;
        }
        strat_futex_wait(&start, 0, 100);
    }
    if (!__atomic_load_n(&start, __ATOMIC_ACQUIRE)) {
        close(fd);
        return 1;
    }
    return run_requests(index, fd);
}

int main(int argc, char **argv) {
    if (argc != 2) return 1;
    for (const char *p = argv[1]; *p; p++) {
        if (*p < '0' || *p > '9') return 1;
        keys = keys * 10 + (unsigned)(*p - '0');
        if (keys > MAX_KEYS) return 1;
    }
    if (keys < CLIENTS) return 1;

    uint32_t workers[CLIENTS];
    unsigned launched = 0;
    for (; launched < CLIENTS; launched++) {
        int64_t tid = strat_thread_create(worker, (void *)(uintptr_t)launched, NULL);
        if (tid < 0) {
            __atomic_store_n(&failed, 1, __ATOMIC_RELEASE);
            break;
        }
        workers[launched] = (uint32_t)tid;
    }
    unsigned errors = 0;
    for (unsigned i = 0; i < launched; i++) {
        int status = 0;
        if (strat_thread_join(workers[i], &status) || status) errors++;
    }
    if (errors || completed != keys * OPS_PER_KEY) {
        u_dprintf(2, "ledger-campaign: failed completed=%u clients=%u errors=%u\n", completed, arrived, errors);
        return 1;
    }
    u_dprintf(2, "[PASS] ledger requests=%u records=%u clients=%u attempts=%u\n",
              completed, keys, CLIENTS, attempts);
    return 0;
}
