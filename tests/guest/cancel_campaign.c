#include <stdbool.h>
#include <shared/errno.h>
#include <user/strat_api.h>

extern void u_printf(const char *, ...);

#define WORKERS 9U
#define TARGET_ROUNDS 1112U
#define EXIT_ROUNDS 256U

typedef struct {
    int pipes[2][2];
    int channels[2][2];
    int socket_fd;
    uint32_t futex;
    unsigned entered;
    unsigned exit_now;
    uint32_t tids[WORKERS];
} fixture_t;

typedef struct {
    fixture_t *fixture;
    unsigned mode;
} job_t;

static int worker(void *opaque) {
    job_t *job = opaque;
    fixture_t *fixture = job->fixture;
    char token = 'x';
    __atomic_add_fetch(&fixture->entered, 1, __ATOMIC_RELEASE);
    switch (job->mode) {
        case 0: read(fixture->pipes[0][0], &token, 1); break;
        case 1: write(fixture->pipes[1][1], &token, 1); break;
        case 2: read(fixture->channels[0][0], &token, 1); break;
        case 3: write(fixture->channels[1][1], &token, 1); break;
        case 4: {
            struct pollfd wait = { .fd = fixture->pipes[0][0], .events = POLLIN };
            poll(&wait, 1, -1);
            break;
        }
        case 5: strat_futex_wait(&fixture->futex, 0, 60000); break;
        case 6: sleep_ms(600000); break;
        case 7: for (;;) __asm__ volatile("pause");
        case 8: {
            struct pollfd wait = { .fd = fixture->socket_fd, .events = POLLIN };
            poll(&wait, 1, -1);
            break;
        }
        case 9:
            while (!__atomic_load_n(&fixture->exit_now, __ATOMIC_ACQUIRE)) yield();
            exit(77);
    }
    return 91;
}

static int setup(fixture_t *fixture, job_t jobs[WORKERS + 1]) {
    if (pipe(fixture->pipes[0]) || pipe(fixture->pipes[1]) ||
        strat_channel_pair(fixture->channels[0], 0) || strat_channel_pair(fixture->channels[1], 0)) return 1;
    char fill[4096] = {0};
    if (write(fixture->pipes[1][1], fill, sizeof(fill)) != sizeof(fill)) return 2;
    for (unsigned i = 0; i < STRAT_CHANNEL_QUEUE_CAPACITY; i++) {
        if (write(fixture->channels[1][1], fill, 1) != 1) return 3;
    }
    fixture->socket_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fixture->socket_fd < 0) return 4;

    for (unsigned i = 0; i < WORKERS; i++) {
        jobs[i] = (job_t){ .fixture = fixture, .mode = i };
        int64_t tid = strat_thread_create(worker, &jobs[i], NULL);
        if (tid < 0) return 5;
        fixture->tids[i] = (uint32_t)tid;
    }
    uint64_t deadline = uptime() + 5000;
    while (__atomic_load_n(&fixture->entered, __ATOMIC_ACQUIRE) != WORKERS) {
        if (uptime() >= deadline) return 6;
        yield();
    }
    return 0;
}

static int targeted(unsigned round) {
    fixture_t fixture = {0};
    job_t jobs[WORKERS + 1];
    int result = setup(&fixture, jobs);
    if (result) return result;
    for (unsigned i = 0; i < WORKERS; i++) {
        unsigned at = (i + round) % WORKERS;
        if (strat_thread_cancel(fixture.tids[at])) return 7;
        if (round & 1) yield();
    }
    for (unsigned i = 0; i < WORKERS; i++) {
        int status = 0;
        if (strat_thread_join(fixture.tids[i], &status) || status != -STRATUM_EINTR ||
            strat_thread_cancel(fixture.tids[i]) != -STRATUM_ESRCH) return 8;
    }
    for (unsigned pair = 0; pair < 2; pair++) {
        for (unsigned end = 0; end < 2; end++) {
            if (close(fixture.pipes[pair][end]) || close(fixture.channels[pair][end])) return 9;
        }
    }
    return close(fixture.socket_fd);
}

static int group_exit(unsigned cpu, bool from_worker) {
    int child = strat_fork_on_cpu(cpu);
    if (child < 0) return 1;
    if (!child) {
        fixture_t fixture = {0};
        job_t jobs[WORKERS + 1];
        if (setup(&fixture, jobs)) exit(92);
        if (!from_worker) exit(77);
        jobs[WORKERS] = (job_t){ .fixture = &fixture, .mode = 9 };
        int64_t tid = strat_thread_create(worker, &jobs[WORKERS], NULL);
        if (tid < 0) exit(93);
        __atomic_store_n(&fixture.exit_now, 1, __ATOMIC_RELEASE);
        if (!strat_thread_join((uint32_t)tid, NULL)) exit(94);
        exit(95);
    }
    int status = -1;
    return waitpid(child, &status, 0) == child && status == 77 ? 0 : 2;
}

typedef struct {
    int fd;
    bool replace;
    unsigned entered;
} socket_race_t;

static int watch_socket(void *opaque) {
    socket_race_t *race = opaque;
    struct pollfd wait = { .fd = race->fd, .events = POLLIN };
    __atomic_store_n(&race->entered, 1, __ATOMIC_RELEASE);
    int result = poll(&wait, 1, race->replace ? 5000 : 1);
    if (race->replace) return result == 1 && wait.revents == POLLNVAL ? 0 : 1;
    return result || wait.revents ? 2 : 0;
}

static int socket_races(void) {
    for (unsigned i = 0; i < 2500; i++) {
        socket_race_t race = { .fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP), .replace = i % 2 == 0 };
        if (race.fd < 0) return 1;
        int64_t tid = strat_thread_create(watch_socket, &race, NULL);
        if (tid < 0) return 2;
        while (!__atomic_load_n(&race.entered, __ATOMIC_ACQUIRE)) yield();
        if (race.replace) {
            yield();
            if (dup2(1, race.fd) != race.fd) return 3;
        }
        int status = -1;
        if (strat_thread_join((uint32_t)tid, &status) || status || close(race.fd)) return 4;
    }
    return 0;
}

static int orphan_reap(unsigned cpu, uint32_t baseline) {
    int check[2];
    if (pipe(check)) return 1;
    int child = strat_fork_on_cpu(cpu);
    if (child < 0) return 2;
    if (!child) {
        close(check[0]);
        int grandchild = fork();
        if (grandchild < 0) exit(3);
        if (grandchild) exit(0);
        sleep_ms(20);
        exit(write(check[1], "x", 1) == 1 ? 0 : 4);
    }
    char token;
    int status = -1;
    if (close(check[1]) || waitpid(child, &status, 0) != child || status ||
        read(check[0], &token, 1) != 1 || token != 'x' || close(check[0])) return 3;
    uint64_t deadline = uptime() + 1000;
    do {
        strat_sysinfo_t info;
        if (sysinfo(&info)) return 4;
        if (info.active_processes == baseline) return 0;
        yield();
    } while (uptime() < deadline);
    return 5;
}

static int snapshot(strat_sysinfo_t *info, strat_resources_t *resources) {
    return sysinfo(info) || strat_resources(resources);
}

int main(void) {
    strat_sysinfo_t before, after;
    strat_resources_t resource_before, resource_after;
    int warm = targeted(0);
    int parent_exit = warm ? -1 : group_exit(0, false);
    int worker_exit = parent_exit ? -1 : group_exit(1, true);
    if (warm || parent_exit || worker_exit || snapshot(&before, &resource_before)) {
        u_printf("cancel-campaign: warmup targeted=%d parent_exit=%d worker_exit=%d\n",
                 warm, parent_exit, worker_exit);
        return 1;
    }
    for (unsigned i = 0; i < TARGET_ROUNDS; i++) {
        int result = targeted(i);
        if (result) {
            u_printf("cancel-campaign: target round=%u result=%d\n", i, result);
            return 1;
        }
        if ((i + 1) % 250 == 0) u_printf("cancel-campaign: target rounds=%u\n", i + 1);
    }
    for (unsigned i = 0; i < EXIT_ROUNDS; i++) {
        int result = group_exit(i % before.online_cpus, (i & 1) != 0);
        if (result) {
            u_printf("cancel-campaign: group round=%u result=%d\n", i, result);
            return 1;
        }
    }
    if (socket_races()) return 1;
    for (unsigned i = 0; i < 32; i++) {
        if (orphan_reap(i % before.online_cpus, before.active_processes)) return 1;
    }
    u_printf("[PASS] Socket poll races=2500 and orphan process reaps=32\n");
    if (snapshot(&after, &resource_after) || before.free_memory_bytes != after.free_memory_bytes ||
        before.active_processes != after.active_processes || resource_before.threads != resource_after.threads ||
        resource_before.file_objects != resource_after.file_objects) {
        u_printf("cancel-campaign: before pages=%lu threads=%lu files=%lu after pages=%lu threads=%lu files=%lu\n",
                 before.free_memory_bytes, resource_before.threads, resource_before.file_objects,
                 after.free_memory_bytes, resource_after.threads, resource_after.file_objects);
        return 1;
    }
    u_printf("[PASS] cancellations=%u group_exits=%u CPUs=%u with pages, threads, and files reconciled\n",
             TARGET_ROUNDS * WORKERS, EXIT_ROUNDS, before.online_cpus);
    return 0;
}
