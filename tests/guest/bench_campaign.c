#include <user/strat_api.h>

extern void u_printf(const char *, ...);

#define SHORT_RUNS 30
#define LONG_RUNS 10
#define WARMUP 3
#define TIMER_CALLS 1000
#define PID_CALLS 10000
#define PIPE_TURNS 2000
#define PAGES 256
#define BLOCK 4096
#define READ_BLOCKS 32

static int outgoing[2], incoming[2];
static unsigned char block[BLOCK];

static uint64_t counter(void) {
    unsigned low, high;
    __asm__ volatile("lfence; rdtsc; lfence" : "=a"(low), "=d"(high) :: "memory");
    return ((uint64_t)high << 32) | low;
}

static void sample(const char *name, unsigned run, uint64_t start, unsigned work) {
    uint64_t end = counter();
    if (end <= start) exit(2);
    u_printf("[BENCH] %s run=%u work=%u ticks=%lu\n", name, run, work, end - start);
}

static int relay(void *arg) {
    (void)arg;
    for (unsigned i = 0; i < PIPE_TURNS; i++) {
        unsigned token;
        if (read(outgoing[0], &token, sizeof(token)) != sizeof(token) || token != i) return 1;
        if (write(incoming[1], &token, sizeof(token)) != sizeof(token)) return 2;
    }
    return 0;
}

static int short_runs(void) {
    int pid = getpid();
    for (unsigned run = 0; run < SHORT_RUNS + WARMUP; run++) {
        uint64_t start = counter();
        for (unsigned i = 0; i < TIMER_CALLS; i++) (void)counter();
        if (run >= WARMUP) sample("timer", run - WARMUP, start, TIMER_CALLS);
        start = counter();
        for (unsigned i = 0; i < PID_CALLS; i++) if (getpid() != pid) return 1;
        if (run >= WARMUP) sample("getpid", run - WARMUP, start, PID_CALLS);

        int64_t region = mmap_anonymous(PAGES * BLOCK, PROT_READ | PROT_WRITE);
        if (region < 0) return 2;
        volatile unsigned char *memory = (volatile unsigned char *)(uintptr_t)region;
        start = counter();
        for (unsigned i = 0; i < PAGES; i++) {
            if (memory[i * BLOCK]) return 3;
            memory[i * BLOCK] = (unsigned char)i;
        }
        if (run >= WARMUP) sample("demand_zero", run - WARMUP, start, PAGES);
        if (munmap((void *)(uintptr_t)region, PAGES * BLOCK)) return 4;

        if (pipe(outgoing) || pipe(incoming)) return 5;
        int64_t worker = strat_thread_create(relay, NULL, NULL);
        if (worker < 0) return 6;
        start = counter();
        for (unsigned i = 0; i < PIPE_TURNS; i++) {
            unsigned token = 0;
            if (write(outgoing[1], &i, sizeof(i)) != sizeof(i) ||
                read(incoming[0], &token, sizeof(token)) != sizeof(token) || token != i) return 7;
        }
        if (run >= WARMUP) sample("pipe_roundtrip", run - WARMUP, start, PIPE_TURNS);
        int status;
        if (strat_thread_join((uint32_t)worker, &status) || status) return 9;
        if (close(outgoing[0]) || close(outgoing[1]) || close(incoming[0]) || close(incoming[1])) return 10;
    }
    return 0;
}

static unsigned char pattern(unsigned run, unsigned page, unsigned index) {
    return (unsigned char)(run * 13 + page * 17 + index * 31 + index / 256);
}

static int storage_runs(void) {
    int fd = open("/bench-payload", O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0) return 1;
    for (unsigned run = 0; run < LONG_RUNS + WARMUP; run++) {
        if (lseek(fd, 0, SEEK_SET)) return 2;
        uint64_t start = counter();
        for (unsigned page = 0; page < READ_BLOCKS; page++) {
            for (unsigned i = 0; i < BLOCK; i++) block[i] = pattern(run, page, i);
            if (write(fd, block, BLOCK) != BLOCK) return 3;
        }
        if (run >= WARMUP) sample("block_write", run - WARMUP, start, READ_BLOCKS * BLOCK);
        start = counter();
        if (fsync(fd)) return 4;
        if (run >= WARMUP) sample("fsync", run - WARMUP, start, 1);
        if (lseek(fd, 0, SEEK_SET)) return 5;
        start = counter();
        for (unsigned page = 0; page < READ_BLOCKS; page++) {
            if (read(fd, block, BLOCK) != BLOCK) return 6;
            for (unsigned i = 0; i < BLOCK; i++) if (block[i] != pattern(run, page, i)) return 7;
        }
        if (run >= WARMUP) sample("block_read", run - WARMUP, start, READ_BLOCKS * BLOCK);
    }
    return close(fd) || unlink("/bench-payload");
}

int main(void) {
    int child = strat_fork_on_cpu(0);
    if (!child) {
        int result = short_runs();
        if (result) return 10 + result;
        result = storage_runs();
        return result ? 30 + result : 0;
    }
    int status = -1;
    if (child < 0 || waitpid(child, &status, 0) != child || status) {
        u_printf("bench-campaign: worker status=%d\n", child < 0 ? child : status);
        return 1;
    }
    u_printf("[PASS] Guest benchmark samples verified\n");
    return 0;
}
