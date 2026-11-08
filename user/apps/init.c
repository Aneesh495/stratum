#include <stdbool.h>
#include <user/strat_api.h>

extern int strcmp(const char *, const char *);
extern int sh_execute_cmd(const char *cmd_line);
extern void u_printf(const char *fmt, ...);

static int shell(void) {
    char line[128];
    size_t len = 0;
    bool discard = false;
    bool last_cr = false;

    u_printf("Stratum serial shell. Type help for commands.\nstratum> ");
    for (;;) {
        char ch;
        int64_t n = read(0, &ch, 1);
        if (n < 0) return 1;
        if (n == 0) return 0;
        if (ch == '\n' && last_cr) {
            last_cr = false;
            continue;
        }
        last_cr = ch == '\r';
        if (ch == '\r' || ch == '\n') {
            write(1, "\n", 1);
            if (discard) {
                u_printf("sh: line too long\n");
            } else {
                line[len] = 0;
                sh_execute_cmd(line);
            }
            len = 0;
            discard = false;
            u_printf("stratum> ");
        } else if (ch == '\b' || ch == 127) {
            if (!discard && len > 0) {
                len--;
                write(1, "\b \b", 3);
            }
        } else if (ch >= ' ' && ch <= '~' && !discard) {
            if (len < sizeof(line) - 1) {
                line[len++] = ch;
                write(1, &ch, 1);
            } else {
                discard = true;
            }
        }
    }
}


int main(int argc, const char *const *argv) {
    if (argc > 0 && !strcmp(argv[0], "sh")) return shell();
    static const char *const args[] = {"sh", NULL};
    for (;;) {
        int pid = fork();
        if (!pid) {
            int result = execve("/bin/sh", args, NULL);
            u_printf("init: shell exec failed (%d)\n", result);
            return 127;
        }
        int status = 0;
        if (pid > 0 && waitpid(pid, &status, 0) == pid) {
            u_printf("[INIT] Reaped shell pid=%d status=%d\n", pid, status);
        } else {
            u_printf("init: shell launch or wait failed\n");
        }
        sleep_ms(100);
    }
}
