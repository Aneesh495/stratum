#include <stdbool.h>
#include <shared/errno.h>
#include <user/strat_api.h>

extern void u_printf(const char *, ...);

#define EVENTS 4096U
#define CAPACITY 1024U

static int check_wait(unsigned cpu, uint32_t pid, uint32_t tid) {
    strat_trace_info_t before, after;
    if (strat_trace_info(cpu, &before) || sleep_ms(20) || strat_trace_info(cpu, &after)) return 1;
    bool observed_wait = false;
    for (uint64_t at = before.next; at < after.next; at++) {
        strat_trace_record_t event;
        if (strat_trace_read(cpu, at, &event) || event.args[0] != tid) continue;
        if (event.event == STRAT_TRACE_WAIT && event.tid == tid && event.pid == pid) observed_wait = true;
        if (event.event == STRAT_TRACE_WAKE && observed_wait) return 0;
    }
    return 1;
}

static int campaign(void) {
    unsigned cpu = (unsigned)strat_getcpu();
    uint32_t pid = (uint32_t)getpid(), tid = strat_thread_self();
    if (!tid || check_wait(cpu, pid, tid)) return 7;
    u_printf("[PASS] trace wait and wake correlated pid=%u tid=%u\n", pid, tid);
    strat_trace_info_t before, after;
    if (strat_trace_info(cpu, &before) || strat_trace_info(0xffffffffU, &after) != -STRATUM_EINVAL) return 1;
    for (unsigned i = 0; i < EVENTS; i++) trace_emit(STRAT_TRACE_USER_BASE, i, i ^ 0x55U, i * 7U, 20260426);
    if (strat_trace_info(cpu, &after) || after.next < before.next + EVENTS ||
        after.next - after.first != CAPACITY || after.lost < EVENTS - CAPACITY) return 2;
    strat_trace_record_t record;
    if (strat_trace_read(cpu, after.first - 1, &record) != -STRATUM_ERANGE) return 3;
    unsigned verified = 0, newest = 0;
    uint64_t last_tsc = 0;
    for (uint64_t at = after.first; at < after.next; at++) {
        int result = strat_trace_read(cpu, at, &record);
        if (result == -STRATUM_EAGAIN || result == -STRATUM_ERANGE) continue;
        if (result || record.cpu != cpu || record.tsc < last_tsc) return 4;
        last_tsc = record.tsc;
        if (record.event != STRAT_TRACE_USER_BASE) continue;
        unsigned id = (unsigned)record.args[0];
        if (record.pid != pid || record.tid != tid || id >= EVENTS || (verified && id != newest + 1) ||
            record.args[1] != (id ^ 0x55U) || record.args[2] != id * 7U ||
            record.args[3] != 20260426) return 5;
        newest = id;
        verified++;
    }
    if (newest != EVENTS - 1 || verified < 900) return 6;
    u_printf("[PASS] trace overflow cpu=%u retained=%u lost=%lu verified=%u newest=%u\n",
             cpu, CAPACITY, after.lost, verified, newest);
    return 0;
}


int main(void) {
    int child = strat_fork_on_cpu(0);
    if (!child) return campaign();
    int status = -1;
    if (child < 0 || waitpid(child, &status, 0) != child || status) return 1;
    return 0;
}
