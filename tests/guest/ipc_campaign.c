#include <stdbool.h>
#include <shared/errno.h>
#include <user/strat_api.h>

extern void u_printf(const char *, ...);
extern int strcmp(const char *, const char *);

#define TRANSFERS 50000
#define STREAM_MAX 799

typedef struct {
    int fd;
    bool channel;
} sender_t;

static size_t message_size(unsigned sequence, bool channel) {
    return channel ? 32 + sequence % (STRAT_CHANNEL_MAX_MESSAGE - 31) : 768 + sequence % 32;
}

static uint8_t pattern(unsigned sequence, unsigned offset) {
    return (uint8_t)((sequence ^ (sequence >> 8)) * 13 + offset * 17 + (offset >> 8));
}

static int sender(void *opaque) {
    sender_t *send = opaque;
    uint8_t buffer[STREAM_MAX];
    for (unsigned seq = 0; seq < TRANSFERS; seq++) {
        size_t len = message_size(seq, send->channel);
        for (unsigned j = 0; j < len; j++) buffer[j] = pattern(seq, j);
        if (write(send->fd, buffer, len) != (int64_t)len) {
            close(send->fd);
            return 1;
        }
    }
    return close(send->fd);
}

static int verify_message(int fd, unsigned seq, bool channel, uint64_t *hash) {
    uint8_t buffer[STREAM_MAX];
    size_t len = message_size(seq, channel);
    size_t received = 0;
    while (received < len) {
        size_t chunk = channel ? sizeof(buffer) : len - received;
        int64_t n = read(fd, buffer + received, chunk);
        if (n <= 0 || (channel && n != (int64_t)len)) return 1;
        received += (size_t)n;
    }
    for (unsigned i = 0; i < len; i++) {
        if (buffer[i] != pattern(seq, i)) return 1;
        *hash = (*hash ^ buffer[i]) * 1099511628211ULL;
    }
    return 0;
}

static int transfer(bool channel) {
    int pair[2];
    if (channel ? strat_channel_pair(pair, 0) : pipe(pair)) return 1;
    sender_t send = { .fd = pair[1], .channel = channel };
    int64_t tid = strat_thread_create(sender, &send, NULL);
    if (tid < 0) return 2;

    uint64_t hash = 14695981039346656037ULL;
    for (unsigned seq = 0; seq < TRANSFERS; seq++) {
        if (verify_message(pair[0], seq, channel, &hash)) {
            u_printf("ipc-campaign: payload mismatch sequence=%u channel=%u\n", seq, channel);
            return 3;
        }
    }

    char byte;
    int status = -1;
    if (read(pair[0], &byte, 1) != 0 || close(pair[0]) || strat_thread_join((uint32_t)tid, &status) || status) return 4;
    u_printf("[PASS] %s transfers=%u payload_fnv64=%lx\n", channel ? "channel" : "pipe", TRANSFERS, hash);
    return 0;
}

static int nonblocking(void) {
    char bytes[STRAT_CHANNEL_MAX_MESSAGE + 1] = { 's', 't', 'r', 'a', 't' };
    int pair[2];
    if (strat_channel_pair(pair, O_NONBLOCK | O_CLOEXEC) ||
        read(pair[0], bytes, sizeof(bytes)) != -STRATUM_EAGAIN ||
        write(pair[1], bytes, sizeof(bytes)) != -STRATUM_EMSGSIZE) return 1;
    for (unsigned i = 0; i < STRAT_CHANNEL_QUEUE_CAPACITY; i++) {
        if (write(pair[1], bytes, 5) != 5) return 2;
    }
    if (write(pair[1], bytes, 5) != -STRATUM_EAGAIN ||
        read(pair[0], (void *)1, 5) != -STRATUM_EFAULT ||
        read(pair[0], bytes, 4) != -STRATUM_EMSGSIZE) return 3;
    for (unsigned i = 0; i < STRAT_CHANNEL_QUEUE_CAPACITY; i++) {
        char payload[5];
        if (read(pair[0], payload, sizeof(payload)) != sizeof(payload)) return 4;
        for (unsigned j = 0; j < sizeof(payload); j++) if (payload[j] != bytes[j]) return 5;
    }
    if (write(pair[0], bytes, 5) != 5 || close(pair[0]) ||
        write(pair[1], bytes, 1) != -STRATUM_EPIPE ||
        read(pair[1], bytes, sizeof(bytes)) != 5 ||
        read(pair[1], bytes, sizeof(bytes)) != 0 || close(pair[1])) return 6;

    if (pipe2(pair, O_NONBLOCK) || read(pair[0], bytes, 1) != -STRATUM_EAGAIN) return 7;
    for (unsigned i = 0; i < 8; i++) if (write(pair[1], bytes, 512) != 512) return 8;
    if (read(pair[0], bytes, 1) != 1 || write(pair[1], bytes, 2) != -STRATUM_EAGAIN ||
        write(pair[1], bytes, 1) != 1 || close(pair[0]) ||
        write(pair[1], bytes, 1) != -STRATUM_EPIPE || close(pair[1])) return 9;
    return 0;
}

typedef struct {
    int pair[2];
    unsigned type;
    uint32_t started;
} watch_t;

static int watch(void *opaque) {
    watch_t *test = opaque;
    static const short events[] = { POLLIN, POLLIN | POLLHUP, POLLNVAL, 0 };
    struct pollfd fd = { .fd = test->pair[0], .events = POLLIN };
    __atomic_store_n(&test->started, 1, __ATOMIC_RELEASE);
    int result = poll(&fd, 1, test->type == 3 ? 1 : 5000);
    return result == (events[test->type] ? 1 : 0) && fd.revents == events[test->type] ? 73 : 1;
}

static int races(void) {
    enum { PER_TYPE = 6250, GROUP = 4 };
    for (unsigned base = 0; base < PER_TYPE; base += GROUP) {
        unsigned group = PER_TYPE - base < GROUP ? PER_TYPE - base : GROUP;
        watch_t watches[4 * GROUP] = {0};
        uint32_t tids[4 * GROUP];
        for (unsigned i = 0; i < group * 4; i++) {
            watch_t *test = &watches[i];
            test->type = i / group;
            if (strat_channel_pair(test->pair, 0)) return 1;
            int64_t tid = strat_thread_create(watch, test, NULL);
            if (tid < 0) return 2;
            tids[i] = (uint32_t)tid;
        }
        for (unsigned i = 0; i < group * 4; i++) {
            while (!__atomic_load_n(&watches[i].started, __ATOMIC_ACQUIRE)) yield();
        }
        /* Each original descriptor is observed before its replacement. */
        yield();
        for (unsigned i = 0; i < group * 4; i++) {
            watch_t *test = &watches[i];
            if (test->type == 0 && write(test->pair[1], "x", 1) != 1) return 3;
            if (test->type == 1 && close(test->pair[1])) return 4;
            if (test->type == 2 && dup2(1, test->pair[0]) != test->pair[0]) return 5;
        }
        for (unsigned i = 0; i < group * 4; i++) {
            watch_t *test = &watches[i];
            int status = -1;
            if (strat_thread_join(tids[i], &status) || status != 73) {
                u_printf("ipc-campaign: readiness case=%u type=%u status=%d\n", base, test->type, status);
                return 6;
            }
            if (close(test->pair[0])) return 7;
            if (test->type != 1 && close(test->pair[1])) return 8;
        }
    }
    u_printf("[PASS] readiness cases=25000 data=6250 peer_close=6250 descriptor_replace=6250 timeout=6250\n");
    return 0;
}

int main(void) {
    int status = nonblocking();
    if (status) {
        u_printf("ipc-campaign: nonblocking contract failure=%d\n", status);
        return 1;
    }
    /* Warm worker stack tables before comparing quiescent page counts. */
    int pair[2];
    if (strat_channel_pair(pair, 0)) return 1;
    watch_t warm = { .pair = { pair[0], pair[1] }, .type = 3 };
    int64_t tid = strat_thread_create(watch, &warm, NULL);
    if (tid < 0 || strat_thread_join((uint32_t)tid, &status) || status != 73 ||
        close(pair[0]) || close(pair[1])) return 1;

    strat_sysinfo_t before, after;
    if (sysinfo(&before)) return 1;
    status = transfer(false);
    if (!status) status = transfer(true);
    if (!status) status = races();
    if (status) {
        u_printf("ipc-campaign: failed status=%d\n", status);
        return 1;
    }
    if (sysinfo(&after) || before.active_processes != after.active_processes) return 1;
    /* The 16-worker race creates guarded stacks within the same page table. */
    if (before.free_memory_bytes != after.free_memory_bytes) {
        u_printf("ipc-campaign: free before=%lu after=%lu\n", before.free_memory_bytes, after.free_memory_bytes);
        return 1;
    }
    for (int fd = 3; fd < 64; fd++) {
        if (open("/bin/true", O_RDONLY, 0) != fd) return 1;
    }
    if (open("/bin/true", O_RDONLY, 0) != -STRATUM_EMFILE) return 1;
    for (int fd = 3; fd < 64; fd++) if (close(fd)) return 1;
    u_printf("[PASS] IPC descriptors and free pages reconciled\n");
    return 0;
}
