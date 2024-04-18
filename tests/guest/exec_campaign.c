#include <shared/errno.h>
#include <user/strat_api.h>

extern int strcmp(const char *, const char *);
extern void u_printf(const char *, ...);

#define CAMPAIGN_PATH "/bin/exec-campaign"
#define EXEC_CYCLES 10000

static int probe(int argc, char **argv, char **envp) {
    if (argc != 3 || strcmp(argv[0], CAMPAIGN_PATH) || strcmp(argv[1], "probe") ||
        strcmp(argv[2], "one argument with spaces") || argv[3] ||
        !envp[0] || strcmp(envp[0], "STRATUM_EXEC=verified") || envp[1]) return 1;
    char magic[6];
    if (read(3, magic, sizeof(magic)) != sizeof(magic) || magic[0] != 0x7f ||
        magic[1] != 'E' || magic[5] != 1 || read(4, magic, sizeof(magic)) != -STRATUM_EBADF) return 2;
    return 73;
}

static int cycle(void) {
    const char *argv[] = { CAMPAIGN_PATH, "probe", "one argument with spaces", NULL };
    const char *envp[] = { "STRATUM_EXEC=verified", NULL };
    int pid = fork();
    if (pid < 0) return pid;
    if (pid == 0) {
        if (open(CAMPAIGN_PATH, O_RDONLY, 0) != 3 ||
            open(CAMPAIGN_PATH, O_RDONLY | O_CLOEXEC, 0) != 4) exit(3);
        int result = execve(CAMPAIGN_PATH, argv, envp);
        u_printf("exec-campaign: exec returned %d\n", result);
        exit(4);
    }
    int status = -1;
    if (waitpid(pid, &status, 0) != pid) return -1;
    return status == 73 ? 0 : (status ? status : -1);
}

int main(int argc, char **argv, char **envp) {
    if (argc > 1) return probe(argc, argv, envp);
    const char *too_many[34];
    for (size_t i = 0; i < 33; i++) too_many[i] = "x";
    too_many[33] = NULL;
    if (execve(CAMPAIGN_PATH, too_many, NULL) != -STRATUM_E2BIG ||
        execve(CAMPAIGN_PATH, (void *)1, NULL) != -STRATUM_EFAULT ||
        execve("/bin/missing", NULL, NULL) != -STRATUM_ENOENT ||
        execve("/bin", NULL, NULL) != -STRATUM_ENOEXEC) {
        u_printf("exec-campaign: invalid input check failed\n");
        return 1;
    }

    strat_sysinfo_t before, after;
    int status = cycle();
    if (status || sysinfo(&before)) {
        u_printf("exec-campaign: warmup status=%d\n", status);
        return 1;
    }
    for (int i = 0; i < EXEC_CYCLES; i++) {
        status = cycle();
        if (status) {
            u_printf("exec-campaign: cycle=%d status=%d\n", i, status);
            return 1;
        }
        if ((i + 1) % 1000 == 0) u_printf("exec-campaign: completed=%d\n", i + 1);
    }
    if (sysinfo(&after) || before.free_memory_bytes != after.free_memory_bytes ||
        before.active_processes != after.active_processes) {
        u_printf("exec-campaign: resources changed after reaping\n");
        return 1;
    }
    u_printf("[PASS] fork/exec/wait cycles=%d with argv, envp, CLOEXEC and resource recovery\n", EXEC_CYCLES);
    return 0;
}
