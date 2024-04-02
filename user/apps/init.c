#include <stdbool.h>
#include <user/strat_api.h>

extern int sh_execute_cmd(const char *cmd_line);
extern void u_printf(const char *fmt, ...);

int main(void) {
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
