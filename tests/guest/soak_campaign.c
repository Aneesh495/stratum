#include <shared/errno.h>
#include <user/strat_api.h>

extern int memcmp(const void *, const void *, size_t);
extern void u_printf(const char *, ...);

#define CPU_COUNT 8
#define ROUNDS 4
#define PAGE 4096
#define PAGES 8
#define UDP_COUNT "128"

typedef struct {
    int writer;
    uint64_t token;
} message_t;

static int send_token(void *arg) {
    const message_t *message = arg;
    return write(message->writer, &message->token, sizeof(message->token)) ==
        sizeof(message->token) ? 0 : 1;
}

static int transfer(uint64_t token, unsigned round) {
    int pair[2];
    if ((round & 1) ? strat_channel_pair(pair, 0) : pipe(pair)) return 1;
    message_t message = { .writer = pair[1], .token = token };
    int64_t tid = strat_thread_create(send_token, &message, NULL);
    if (tid < 0) return 2;
    struct pollfd event = { .fd = pair[0], .events = POLLIN };
    if (poll(&event, 1, 3000) != 1 || event.revents != POLLIN) return 3;
    uint64_t got = 0;
    if (read(pair[0], &got, sizeof(got)) != sizeof(got) || got != token) return 4;
    int status = -1;
    if (strat_thread_join((uint32_t)tid, &status) || status ||
        close(pair[0]) || close(pair[1])) return 5;
    return 0;
}

static int disk_round(const char *path, const uint8_t *data) {
    uint8_t actual[PAGE];
    int fd = open(path, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0) return 1;
    if (write(fd, data, PAGE) != PAGE || fsync(fd) || lseek(fd, 0, SEEK_SET) != 0) return 2;
    size_t used = 0;
    while (used < sizeof(actual)) {
        int64_t n = read(fd, actual + used, sizeof(actual) - used);
        if (n <= 0) return 3;
        used += (size_t)n;
    }
    if (memcmp(data, actual, PAGE)) return 4;
    if (close(fd) || unlink(path)) return 5;
    return 0;
}

static int worker(unsigned cpu) {
    if (strat_getcpu() != (int)cpu) return 1;
    for (unsigned round = 0; round < ROUNDS; round++) {
        int64_t address = mmap_anonymous(PAGE * PAGES, PROT_READ | PROT_WRITE);
        if (address < 0) return 2;
        volatile uint8_t *pages = (void *)(uintptr_t)address;
        for (unsigned i = 0; i < PAGES; i++) {
            if (pages[i * PAGE] != 0) return 3;
            pages[i * PAGE] = (uint8_t)(i + cpu + round + 1);
        }
        for (unsigned i = 1; i < PAGE; i++) pages[i] = (uint8_t)(i * 13 + cpu * 7 + round);
        int result = transfer((uint64_t)address ^ ((uint64_t)cpu << 32) ^ round, round);
        if (result) return 10 + result;
        if (cpu == 1) {
            result = disk_round("/soak-disk", (const void *)pages);
            if (result) return 20 + result;
        }
        if (munmap((void *)pages, PAGE * PAGES)) return 4;
    }
    return 0;
}

int main(void) {
    int network = strat_fork_on_cpu(0);
    if (network < 0) return 1;
    if (network == 0) {
        const char *args[] = { "/bin/net-campaign", UDP_COUNT, NULL };
        execve(args[0], args, NULL);
        exit(1);
    }

    int children[CPU_COUNT] = {0};
    unsigned started = 0;
    for (; started < CPU_COUNT; started++) {
        int child = strat_fork_on_cpu(started);
        if (child < 0) break;
        if (child == 0) exit(worker(started));
        children[started] = child;
    }
    unsigned mask = 0;
    for (unsigned cpu = 0; cpu < started; cpu++) {
        int status = -1;
        if (waitpid(children[cpu], &status, 0) == children[cpu] && status == 0) {
            mask |= 1U << cpu;
        } else {
            u_printf("soak-campaign: CPU %u status=%d\n", cpu, status);
        }
    }
    int status = -1;
    if (waitpid(network, &status, 0) != network || status || mask != (1U << CPU_COUNT) - 1) return 2;

    /* All descriptor numbers above the inherited console must be available again. */
    for (int fd = 3; fd < 64; fd++) {
        if (open("/bin/true", O_RDONLY, 0) != fd) return 3;
    }
    if (open("/bin/true", O_RDONLY, 0) != -STRATUM_EMFILE) return 4;
    for (int fd = 3; fd < 64; fd++) if (close(fd)) return 5;

    u_printf("[PASS] soak CPUs=0x%x faults=%u processes=%u threads=%u transfers=%u fsyncs=%u fds=61\n",
             mask, CPU_COUNT * ROUNDS * PAGES, CPU_COUNT + 1, CPU_COUNT * ROUNDS,
             CPU_COUNT * ROUNDS, ROUNDS);
    return 0;
}
