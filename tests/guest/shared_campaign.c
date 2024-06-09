#include <shared/errno.h>
#include <user/strat_api.h>

extern void u_printf(const char *, ...);
extern int strcmp(const char *, const char *);

#define PAGE 4096U
#define CYCLES 2000U
#define PAGES 4U
#define REGION (PAGE * PAGES)
#define RW (PROT_READ | PROT_WRITE)

static int reap(int child, int expected) {
    int status = -1;
    return waitpid(child, &status, 0) == child && status == expected ? 0 : 1;
}

static int cycle(unsigned sequence, unsigned cpu) {
    int64_t common = mmap_shared(REGION, PROT_READ);
    int64_t own = mmap_anonymous(PAGE, RW);
    if (common < 0 || own < 0) return 1;

    volatile uint8_t *shared = (void *)(uintptr_t)common;
    volatile uint8_t *private = (void *)(uintptr_t)own;
    private[0] = 19;

    int child = strat_fork_on_cpu(cpu);
    if (child < 0) return 2;
    if (child == 0) {
        if (strat_getcpu() != (int)cpu || mprotect((void *)shared, REGION, RW) ||
            private[0] != 19) exit(3);
        private[0] = 11;
        for (unsigned i = 0; i < PAGES; i++) {
            if (shared[i * PAGE] != 0) exit(4);
            shared[i * PAGE] = (uint8_t)(sequence + i + 1);
        }
        exit(munmap((void *)shared, REGION) ? 5 : 0);
    }
    if (reap(child, 0) || private[0] != 19) return 3;
    for (unsigned i = 0; i < PAGES; i++) {
        if (shared[i * PAGE] != (uint8_t)(sequence + i + 1)) return 4;
    }
    if (sequence == 0) {
        child = strat_fork_on_cpu(cpu);
        if (child < 0) return 5;
        if (child == 0) {
            shared[0] = 0;
            exit(1);
        }
        if (reap(child, 128 + 14) || shared[0] != 1) return 6;
    }
    if (munmap((void *)shared, REGION) || munmap((void *)private, PAGE)) return 7;
    return 0;
}

static int last_owner(void) {
    int64_t result = mmap_shared(PAGE, RW);
    int pair[2];
    if (result < 0 || pipe(pair)) return 1;
    volatile char *page = (void *)(uintptr_t)result;
    *page = 's';
    int child = strat_fork_on_cpu(1);
    if (child < 0) return 2;
    if (child == 0) {
        char message;
        close(pair[1]);
        if (read(pair[0], &message, 1) != 1 || message != 'y' || *page != 's') exit(3);
        *page = 't';
        exit(munmap((void *)page, PAGE) ? 4 : 0);
    }
    if (close(pair[0]) || munmap((void *)page, PAGE) ||
        write(pair[1], "y", 1) != 1 || close(pair[1])) return 3;
    return reap(child, 0);
}

static int exhaust_child(void) {
    const size_t length = 2U * 1024U * 1024U;
    void *mapped[16];
    unsigned count = 0;
    strat_sysinfo_t before, after;
    for (; count < 16; count++) {
        if (sysinfo(&before)) return 1;
        int64_t addr = mmap_shared(length, RW);
        if (addr == -STRATUM_ENOMEM) break;
        if (addr < 0) return 2;
        mapped[count] = (void *)(uintptr_t)addr;
    }
    if (!count || count == 16 || sysinfo(&after) || before.free_memory_bytes >= length ||
        after.free_memory_bytes + 16 * PAGE < before.free_memory_bytes) return 3;
    /* A failed partial allocation retains only bounded page table metadata. */
    int64_t retry = mmap_shared(PAGE, RW);
    if (retry < 0 || munmap((void *)(uintptr_t)retry, PAGE)) return 4;
    for (unsigned i = 0; i < count; i++) if (munmap(mapped[i], length)) return 5;
    u_printf("[EXHAUST] regions=%u free_before=%lu free_after=%lu\n", count,
             before.free_memory_bytes, after.free_memory_bytes);
    return 0;
}

static int exhaust(void) {
    strat_sysinfo_t before, after;
    if (sysinfo(&before)) return 1;
    int child = fork();
    if (child < 0) return 1;
    if (!child) exit(exhaust_child());
    if (reap(child, 0) || sysinfo(&after) || after.free_memory_bytes != before.free_memory_bytes ||
        after.active_processes != before.active_processes) return 1;
    u_printf("[PASS] allocator exhaustion, rollback, retry and final page recovery\n");
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "exhaust")) return exhaust();
    if (argc != 1) return 1;
    strat_sysinfo_t before, after;
    if (sysinfo(&before) || before.online_cpus < 2 || cycle(0, 1) || last_owner() ||
        sysinfo(&before)) return 1;

    for (unsigned i = 1; i <= CYCLES; i++) {
        int result = cycle(i, i % before.online_cpus);
        if (result) {
            u_printf("shared-campaign: case=%u status=%d\n", i, result);
            return 1;
        }
    }
    if (last_owner() || sysinfo(&after) || before.active_processes != after.active_processes ||
        before.free_memory_bytes != after.free_memory_bytes) return 1;
    u_printf("[PASS] shared mappings=%u COW=%u final-owner handoff, protection and exact page recovery\n",
             CYCLES, CYCLES);
    return 0;
}
