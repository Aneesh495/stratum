#include <stdbool.h>
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
#define SWITCH_TURNS 2000
#define WAKE_SAMPLES 4
#define SCALE_WORKERS 8
#define SCALE_ITEMS 250000

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

static int yield_peer(void *arg) {
    (void)arg;
    for (unsigned i = 0; i < SWITCH_TURNS; i++) if (yield()) return 1;
    return 0;
}

static int context_switches(unsigned run) {
    unsigned cpu = (unsigned)strat_getcpu();
    int64_t before = strat_sched_switches(cpu);
    int64_t peer = strat_thread_create(yield_peer, NULL, NULL);
    if (before < 0 || peer < 0) return 1;
    uint64_t start = counter();
    for (unsigned i = 0; i < SWITCH_TURNS; i++) if (yield()) return 2;
    int64_t switched = strat_sched_switches(cpu) - before;
    if (run >= WARMUP) sample("context_switch", run - WARMUP, start, 2 * SWITCH_TURNS);
    int status;
    if (strat_thread_join((uint32_t)peer, &status) || status || switched < 2 * SWITCH_TURNS - 2 ||
        switched > 2 * SWITCH_TURNS + 2) return 3;
    return 0;
}

static int wake_to_run(unsigned run) {
    uint64_t total = 0;
    uint32_t tid = strat_thread_self();
    for (unsigned i = 0; i < WAKE_SAMPLES; i++) {
        strat_trace_info_t before, after;
        if (strat_trace_info(0, &before) || sleep_ms(10) || strat_trace_info(0, &after)) return 1;
        uint64_t woke = 0;
        bool observed = false;
        for (uint64_t at = before.next; at < after.next; at++) {
            strat_trace_record_t event;
            if (strat_trace_read(0, at, &event)) return 2;
            if (event.event == STRAT_TRACE_WAKE && event.args[0] == tid) woke = event.tsc;
            if (event.event == 1 && event.args[1] == tid && woke && event.tsc >= woke) {
                total += event.tsc - woke;
                observed = true;
                break;
            }
        }
        if (!observed) return 3;
    }
    if (run >= WARMUP) {
        if (!total) return 4;
        u_printf("[BENCH] wake_to_run run=%u work=%u ticks=%lu\n", run - WARMUP, WAKE_SAMPLES, total);
    }
    return 0;
}

static int allocator(unsigned run) {
    uint64_t start = counter();
    int64_t region = mmap_shared(PAGES * BLOCK, PROT_READ | PROT_WRITE);
    if (region < 0) return 1;
    volatile unsigned char *bytes = (volatile unsigned char *)(uintptr_t)region;
    for (unsigned page = 0; page < PAGES; page++) if (bytes[page * BLOCK]) return 2;
    if (munmap((void *)(uintptr_t)region, PAGES * BLOCK)) return 3;
    if (run >= WARMUP) sample("allocator", run - WARMUP, start, PAGES);
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
        int switches = context_switches(run);
        if (switches) return 10 + switches;
        int wake = wake_to_run(run);
        if (wake) return 15 + wake;
        if (allocator(run)) return 20;
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
            for (size_t filled = 0; filled < BLOCK;) {
                int64_t received = read(fd, block + filled, BLOCK - filled);
                if (received <= 0) return 6;
                filled += (size_t)received;
            }
            for (unsigned i = 0; i < BLOCK; i++) if (block[i] != pattern(run, page, i)) return 7;
        }
        if (run >= WARMUP) sample("block_read", run - WARMUP, start, READ_BLOCKS * BLOCK);
    }
    return close(fd) || unlink("/bench-payload");
}

typedef struct {
    uint32_t worker;
    uint32_t cpu;
    uint64_t total;
} scale_result_t;

static int scaling(void) {
    strat_sysinfo_t info;
    if (sysinfo(&info) || !info.online_cpus || info.online_cpus > SCALE_WORKERS) return 1;
    for (unsigned run = 0; run < LONG_RUNS + WARMUP; run++) {
        int report[2], children[SCALE_WORKERS];
        if (pipe(report)) return 2;
        uint64_t start = counter();
        for (unsigned i = 0; i < SCALE_WORKERS; i++) {
            children[i] = strat_fork_on_cpu(i % info.online_cpus);
            if (!children[i]) {
                close(report[0]);
                volatile uint64_t sum = 0;
                for (unsigned n = 0; n < SCALE_ITEMS; n++) sum += n + i;
                scale_result_t record = { .worker = i, .cpu = (uint32_t)strat_getcpu(), .total = sum };
                exit(write(report[1], &record, sizeof(record)) == sizeof(record) ? 0 : 1);
            }
            if (children[i] < 0) return 3;
        }
        close(report[1]);
        unsigned seen = 0;
        for (unsigned i = 0; i < SCALE_WORKERS; i++) {
            scale_result_t result;
            if (read(report[0], &result, sizeof(result)) != sizeof(result) || result.worker >= SCALE_WORKERS ||
                result.cpu != result.worker % info.online_cpus || seen & (1U << result.worker) ||
                result.total != (uint64_t)SCALE_ITEMS * (SCALE_ITEMS - 1) / 2 +
                                (uint64_t)result.worker * SCALE_ITEMS) return 4;
            seen |= 1U << result.worker;
        }
        close(report[0]);
        for (unsigned i = 0; i < SCALE_WORKERS; i++) {
            int status;
            if (waitpid(children[i], &status, 0) != children[i] || status) return 5;
        }
        if (run >= WARMUP) sample("smp_scaling", run - WARMUP, start, SCALE_WORKERS * SCALE_ITEMS);
    }
    return 0;
}

int main(void) {
    int child = strat_fork_on_cpu(0);
    if (!child) {
        int result = short_runs();
        if (result) return 10 + result;
        result = storage_runs();
        if (result) return 40 + result;
        result = scaling();
        return result ? 60 + result : 0;
    }
    int status = -1;
    if (child < 0 || waitpid(child, &status, 0) != child || status) {
        u_printf("bench-campaign: worker status=%d\n", child < 0 ? child : status);
        return 1;
    }
    u_printf("[PASS] Guest benchmark samples verified\n");
    return 0;
}
