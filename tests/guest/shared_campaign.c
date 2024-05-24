#include <shared/errno.h>
#include <user/strat_api.h>

extern void u_printf(const char *, ...);

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

int main(void) {
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
