#include <stdbool.h>
#include <shared/errno.h>
#include <user/strat_api.h>

extern int strcmp(const char *, const char *);
extern void u_printf(const char *, ...);

#define PROCESS_COUNT 32
#define WORKERS 16
#define POPULATION (PROCESS_COUNT * WORKERS)
#define LIFECYCLE_CYCLES 25000
#define YIELDS_PER_WORKER 2200
#define MAX_CPUS 32

static int ready_pipe[2], gate_pipe[2];
static int expected_cpu;

typedef struct {
    uint32_t pid, worker, tid, cpu;
} ready_record_t;

static uint64_t fs_read(void) {
    uint64_t value;
    __asm__ volatile("movq %%fs:0, %0" : "=r"(value));
    return value;
}

static uint64_t xmm_read(void) {
    uint64_t value;
    __asm__ volatile("movq %%xmm0, %0" : "=r"(value));
    return value;
}

static void xmm_write(uint64_t value) {
    __asm__ volatile("movq %0, %%xmm0" :: "r"(value));
}

static int lifecycle_worker(void *arg) {
    uint64_t value = *(const uint64_t *)arg;
    uintptr_t fs = 0;
    if (!strat_thread_self() || strat_get_fs(&fs) || fs != (uintptr_t)arg || fs_read() != value) return 1;
    if (strat_getcpu() != expected_cpu || strat_thread_join(strat_thread_self(), NULL) != -STRATUM_EDEADLK) return 2;

    if (xmm_read() != 0) return 3;
    xmm_write(value);
    yield();
    if (xmm_read() != value || fs_read() != value) return 4;
    return 73;
}

static int lifecycle_cycle(unsigned sequence) {
    uint64_t tls = 0x8888000000000000ULL | sequence;
    int64_t tid = strat_thread_create(lifecycle_worker, &tls, &tls);
    if (tid < 0) return (int)tid;
    if (!sequence && (fork() != -STRATUM_EBUSY || execve("/bin/true", NULL, NULL) != -STRATUM_EBUSY)) return 1;
    yield();
    int status = -1;
    if (strat_thread_join((uint32_t)tid, (int *)1) != -STRATUM_EFAULT ||
        strat_thread_join((uint32_t)tid, &status) || status != 73 ||
        strat_thread_join((uint32_t)tid, NULL) != -STRATUM_ESRCH) {
        u_printf("thread-campaign: unexpected join status=%d\n", status);
        return 2;
    }
    return 0;
}

static int lifecycle(void) {
    uint64_t tls = 0x1122334455667788ULL;
    if (strat_set_fs(&tls)) return 1;
    xmm_write(tls);
    expected_cpu = strat_getcpu();
    if (lifecycle_cycle(0)) return 2;
    strat_sysinfo_t before, after;
    if (sysinfo(&before)) return 3;

    for (unsigned i = 1; i <= LIFECYCLE_CYCLES; i++) {
        int res = lifecycle_cycle(i);
        if (res || fs_read() != tls || xmm_read() != tls) {
            u_printf("thread-campaign: lifecycle cycle=%u error=%d\n", i, res);
            return 4;
        }
        if (i % 5000 == 0) u_printf("thread-campaign: completed=%u\n", i);
    }
    if (sysinfo(&after) || before.free_memory_bytes != after.free_memory_bytes ||
        before.active_processes != after.active_processes) return 5;
    u_printf("[PASS] thread create/join cycles=%u with FS TLS, XMM state, stale TIDs and resource recovery\n",
             LIFECYCLE_CYCLES);
    return 0;
}

static int population_worker(void *arg) {
    ready_record_t record = {
        .pid = (uint32_t)getpid(), .worker = (uint32_t)(uintptr_t)arg,
        .tid = strat_thread_self(), .cpu = (uint32_t)strat_getcpu(),
    };
    if (write(ready_pipe[1], &record, sizeof(record)) != sizeof(record)) return 1;
    char generation;
    if (read(gate_pipe[0], &generation, 1) != 1 || generation != 'G') return 2;

    uint64_t started = uptime();
    uint64_t value = (uint64_t)record.pid << 32 | record.tid;
    xmm_write(value);
    for (unsigned i = 0; i < YIELDS_PER_WORKER; i++) {
        if (yield() || xmm_read() != value) return 3;
    }
    return uptime() - started <= 60000 ? 73 : 4;
}

static int populate_child(void) {
    close(ready_pipe[0]);
    close(gate_pipe[1]);
    int64_t tids[WORKERS];
    for (unsigned i = 0; i < WORKERS; i++) {
        tids[i] = strat_thread_create(population_worker, (void *)(uintptr_t)i, NULL);
        if (tids[i] < 0) {
            ready_record_t failure = { .pid = (uint32_t)getpid(), .worker = UINT32_MAX };
            write(ready_pipe[1], &failure, sizeof(failure));
            return 1;
        }
    }
    for (unsigned i = 0; i < WORKERS; i++) {
        int status = -1;
        if (strat_thread_join((uint32_t)tids[i], &status) || status != 73) return 2;
    }
    return 0;
}

static int read_full(int fd, void *buffer, size_t size) {
    size_t done = 0;
    while (done < size) {
        int64_t amount = read(fd, (char *)buffer + done, size - done);
        if (amount <= 0) return -1;
        done += (size_t)amount;
    }
    return 0;
}

static int population(void) {
    /* Warm the allocator's retained pipe slab before measuring free pages. */
    int warm[2];
    if (pipe(warm) || close(warm[0]) || close(warm[1])) return 1;
    strat_sysinfo_t before, after;
    if (sysinfo(&before) || !before.online_cpus || before.online_cpus > MAX_CPUS) return 1;
    int64_t baseline[MAX_CPUS];
    for (unsigned i = 0; i < before.online_cpus; i++) baseline[i] = strat_sched_switches(i);

    if (pipe(ready_pipe) || pipe(gate_pipe)) return 2;
    uint32_t pids[PROCESS_COUNT] = {0};
    unsigned launched = 0;
    int result = 1;
    for (; launched < PROCESS_COUNT; launched++) {
        int pid = strat_fork_on_cpu(launched % before.online_cpus);
        if (pid == 0) exit(populate_child());
        if (pid < 0) goto close_and_wait;
        pids[launched] = (uint32_t)pid;
    }

    close(ready_pipe[1]);
    close(gate_pipe[0]);
    uint32_t masks[PROCESS_COUNT] = {0};
    uint32_t tids[POPULATION];
    uint32_t cpus = 0;
    for (unsigned i = 0; i < POPULATION; i++) {
        ready_record_t record;
        if (read_full(ready_pipe[0], &record, sizeof(record)) ||
            record.cpu >= before.online_cpus || record.worker >= WORKERS || !record.tid) goto close_and_wait;

        unsigned child = 0;
        while (child < PROCESS_COUNT && pids[child] != record.pid) child++;
        if (child == PROCESS_COUNT || (masks[child] & (1U << record.worker))) goto close_and_wait;
        for (unsigned j = 0; j < i; j++) if (tids[j] == record.tid) goto close_and_wait;
        tids[i] = record.tid;
        masks[child] |= 1U << record.worker;
        cpus |= 1U << record.cpu;
        u_printf("[LIVE] pid=%u worker=%u tid=%u cpu=%u\n", record.pid, record.worker, record.tid, record.cpu);
    }
    if (cpus != UINT32_MAX >> (MAX_CPUS - before.online_cpus)) {
        u_printf("thread-campaign: missing CPUs, observed mask=%x\n", cpus);
        goto close_and_wait;
    }
    u_printf("[BARRIER] generation=G live=%u processes=%u cpu_mask=%x\n", POPULATION, PROCESS_COUNT, cpus);
    char generation[POPULATION];
    for (unsigned i = 0; i < POPULATION; i++) generation[i] = 'G';
    if (write(gate_pipe[1], generation, sizeof(generation)) != sizeof(generation)) goto close_and_wait;
    result = 0;

close_and_wait:
    close(ready_pipe[0]);
    close(ready_pipe[1]);
    close(gate_pipe[0]);
    close(gate_pipe[1]);
    for (unsigned i = 0; i < launched; i++) {
        int status = -1;
        if (waitpid((int)pids[i], &status, 0) != (int)pids[i] || status) {
            u_printf("thread-campaign: pid=%u status=%d\n", pids[i], status);
            result = 1;
        }
    }
    if (result) return result;
    if (sysinfo(&after)) return 1;
    if (before.free_memory_bytes != after.free_memory_bytes || before.active_processes != after.active_processes) {
        u_printf("thread-campaign: free before=%lu after=%lu processes before=%u after=%u\n",
                 before.free_memory_bytes, after.free_memory_bytes, before.active_processes, after.active_processes);
        return 1;
    }

    uint64_t total = 0;
    for (unsigned i = 0; i < before.online_cpus; i++) {
        int64_t delta = strat_sched_switches(i) - baseline[i];
        if (delta <= 0) return 1;
        u_printf("[SWITCH] cpu=%u count=%lu\n", i, (uint64_t)delta);
        total += (uint64_t)delta;
    }
    if (total < 1000000) return 1;
    u_printf("[PASS] distinct runnable user switches=%lu completed workers=%u\n", total, POPULATION);
    u_printf("[PASS] Every runnable worker completed within 60000 guest milliseconds\n");
    return 0;
}

static int boundaries(void) {
    enum { PAGE = 4096, CASES = 1000 };
    int64_t address = mmap_anonymous(PAGE, PROT_READ | PROT_WRITE);
    if (address < 0) return 1;
    char *page = (char *)(uintptr_t)address;
    for (unsigned i = PAGE - 1024; i < PAGE; i++) page[i] = 'x';
    int fd = open("/bin/true", O_RDONLY, 0);
    if (fd < 0) return 2;

    const char *valid[] = { "/bin/true", NULL };
    char buffer[16];
    for (unsigned i = 0; i < CASES; i++) {
        uintptr_t unmapped = 0x0000700000000000ULL + i * PAGE;
        uintptr_t non_user = 0x0000800000000000ULL + i * PAGE;
        int64_t actual[] = {
            syscall0(1024 + i),
            read(1024 + i, buffer, sizeof(buffer)),
            write(-1024 - (int)i, buffer, sizeof(buffer)),
            dup2(fd, 1024 + i),
            open((const char *)unmapped, O_RDONLY, 0),
            sysinfo((strat_sysinfo_t *)unmapped),
            syscall2(SYS_arch_prctl, ARCH_SET_FS, non_user),
            poll(NULL, 33 + i, 0),
            execve("/bin/true", valid, (const char *const *)unmapped),
            write(1, page + PAGE - (i % 511 + 1), 512),
        };
        static const int expected[] = {
            -STRATUM_ENOSYS, -STRATUM_EBADF, -STRATUM_EBADF, -STRATUM_EBADF, -STRATUM_EFAULT,
            -STRATUM_EFAULT, -STRATUM_EINVAL, -STRATUM_EINVAL, -STRATUM_EFAULT, -STRATUM_EFAULT,
        };
        for (unsigned j = 0; j < sizeof(expected) / sizeof(*expected); j++) {
            if (actual[j] != expected[j]) {
                u_printf("thread-campaign: boundary case=%u family=%u returned=%ld expected=%d\n",
                         i, j, actual[j], expected[j]);
                return 3;
            }
        }
    }
    if (close(fd) || munmap(page, PAGE)) return 4;
    u_printf("[PASS] syscall boundaries=10000 across ten families\n");
    return 0;
}

typedef struct {
    uint32_t *word;
    uint32_t ready;
    int expected;
    uint32_t timeout_ms;
} race_case_t;

static int race_worker(void *opaque) {
    race_case_t *test = opaque;
    __atomic_store_n(&test->ready, 1, __ATOMIC_RELEASE);
    return strat_futex_wait(test->word, 0, test->timeout_ms) == test->expected ? 73 : 1;
}

static int futex_races(void) {
    enum { PAGE = 4096, GROUP = 4, WAVES = 625 };
    int64_t arena = 0;
    for (unsigned round = 0; round < WAVES; round++) {
        int64_t address = mmap_anonymous(PAGE, PROT_READ | PROT_WRITE);
        if (address < 0 || (round && arena != address)) return 1;
        arena = address;
        uint32_t *mapped = (uint32_t *)(uintptr_t)arena;
        uint32_t words[3 * GROUP];
        race_case_t tests[4 * GROUP];
        uint32_t tids[4 * GROUP];

        for (unsigned i = 0; i < 4 * GROUP; i++) {
            if (i < 3 * GROUP) words[i] = i < GROUP;
            else mapped[i - 3 * GROUP] = 0;
            static const int expected[] = { -STRATUM_EAGAIN, 0, -STRATUM_ETIMEDOUT, -STRATUM_EFAULT };
            tests[i] = (race_case_t){
                .word = i < 3 * GROUP ? &words[i] : &mapped[i - 3 * GROUP],
                .expected = expected[i / GROUP],
                .timeout_ms = i / GROUP == 2 ? 1 : 5000,
            };
            int64_t tid = strat_thread_create(race_worker, &tests[i], NULL);
            if (tid < 0) return 2;
            tids[i] = (uint32_t)tid;
        }
        for (unsigned i = 0; i < 4 * GROUP; i++) {
            while (!__atomic_load_n(&tests[i].ready, __ATOMIC_ACQUIRE)) yield();
        }
        yield();
        for (unsigned i = GROUP; i < 2 * GROUP; i++) {
            __atomic_store_n(&words[i], 1, __ATOMIC_RELEASE);
            if (strat_futex_wake(&words[i], 1) != 1) return 3;
        }
        if (munmap(mapped, PAGE)) return 4;
        for (unsigned i = 0; i < 4 * GROUP; i++) {
            int status = -1;
            if (strat_thread_join(tids[i], &status) || status != 73) {
                u_printf("thread-campaign: futex wave=%u case=%u status=%d\n", round, i, status);
                return 5;
            }
        }
        if (strat_futex_wake(mapped, 4) != 0) return 6;
    }
    u_printf("[PASS] futex cases=%u mismatch=%u wake=%u timeout=%u unmap=%u\n",
             4 * GROUP * WAVES, GROUP * WAVES, GROUP * WAVES, GROUP * WAVES, GROUP * WAVES);
    return 0;
}

static int entry_stacks(void) {
    strat_sysinfo_t info;
    if (sysinfo(&info)) return 1;
    for (unsigned cpu = 0; cpu < info.online_cpus; cpu++) {
        int child = strat_fork_on_cpu(cpu);
        if (!child) {
            uint64_t cookie = 0x53544143;
            __asm__ volatile("int $0xf4" : "+a"(cookie) :: "memory");
            exit(cookie == 0x53544144 && strat_getcpu() == (int)cpu ? 0 : 1);
        }
        int status = -1;
        if (child < 0 || waitpid(child, &status, 0) != child || status) return 2;
    }
    u_printf("[PASS] nested protected stacks on %u CPUs\n", info.online_cpus);
    return 0;
}

#define MIGRATION_CHILDREN 32
#define MIGRATION_DEADLINES 128

typedef struct {
    uint32_t pid;
    uint32_t moved;
    uint32_t cpu_mask;
} timer_record_t;

static void deadline_worker(int report) {
    timer_record_t result = { .pid = (uint32_t)getpid() };
    for (unsigned i = 0; i < MIGRATION_DEADLINES; i++) {
        int cpu_before = strat_getcpu();
        uint32_t delay = 10 * (1 + (i % 4));
        uint64_t start = uptime();
        if (sleep_ms(delay)) exit(1);
        uint64_t elapsed = uptime() - start;
        int cpu_after = strat_getcpu();
        if (cpu_before < 0 || cpu_before >= MAX_CPUS || cpu_after < 0 || cpu_after >= MAX_CPUS ||
            elapsed < delay || elapsed > delay + 2000) exit(2);
        if (cpu_before != cpu_after) result.moved++;
        result.cpu_mask |= (1U << cpu_before) | (1U << cpu_after);
    }
    exit(write(report, &result, sizeof(result)) == sizeof(result) ? 0 : 3);
}

static int deadlines(void) {
    int report[2];
    if (pipe(report)) return 1;
    int children[MIGRATION_CHILDREN];
    for (unsigned i = 0; i < MIGRATION_CHILDREN; i++) {
        children[i] = fork();
        if (!children[i]) {
            close(report[0]);
            deadline_worker(report[1]);
        }
        if (children[i] < 0) return 2;
    }
    close(report[1]);
    unsigned moved = 0, mask = 0;
    bool observed[MIGRATION_CHILDREN] = {0};
    for (unsigned i = 0; i < MIGRATION_CHILDREN; i++) {
        timer_record_t result;
        if (read_full(report[0], &result, sizeof(result))) return 3;
        unsigned child = 0;
        while (child < MIGRATION_CHILDREN && (uint32_t)children[child] != result.pid) child++;
        if (child == MIGRATION_CHILDREN || observed[child] || result.moved > MIGRATION_DEADLINES) return 4;
        observed[child] = true;
        moved += result.moved;
        mask |= result.cpu_mask;
        u_printf("[DEADLINE] pid=%u migrations=%u cpu_mask=%x\n", result.pid, result.moved, result.cpu_mask);
    }
    close(report[0]);
    for (unsigned i = 0; i < MIGRATION_CHILDREN; i++) {
        int status;
        if (waitpid(children[i], &status, 0) != children[i] || status) return 5;
    }
    strat_sysinfo_t info;
    if (sysinfo(&info) || !moved || mask != (1U << info.online_cpus) - 1) return 6;
    u_printf("[PASS] deadlines=%u migrations=%u CPU mask=%x\n",
             MIGRATION_CHILDREN * MIGRATION_DEADLINES, moved, mask);
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "deadlines")) return deadlines();
    if (argc == 2 && !strcmp(argv[1], "entry")) return entry_stacks();
    if (argc != 2) return 1;
    int result;
    if (!strcmp(argv[1], "lifecycle")) result = lifecycle();
    else if (!strcmp(argv[1], "population")) result = population();
    else if (!strcmp(argv[1], "boundaries")) result = boundaries();
    else if (!strcmp(argv[1], "futex")) result = futex_races();
    else return 1;
    if (result) u_printf("thread-campaign: failure=%d\n", result);
    return result;
}
