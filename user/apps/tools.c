#include <user/strat_api.h>

extern int strcmp(const char *, const char *);

static int upper(void) {
    char buf[512];
    int64_t len;
    while ((len = read(0, buf, sizeof(buf))) > 0) {
        for (int64_t i = 0; i < len; i++) {
            if (buf[i] >= 'a' && buf[i] <= 'z') buf[i] -= 'a' - 'A';
        }
        size_t sent = 0;
        while (sent < (size_t)len) {
            int64_t n = write(1, buf + sent, (size_t)len - sent);
            if (n <= 0) return 1;
            sent += (size_t)n;
        }
    }
    return len < 0;
}

int main(int argc, char **argv) {
    if (!argc || !argv[0]) return 2;
    const char *name = argv[0];
    for (const char *p = name; *p; p++) {
        if (*p == '/') name = p + 1;
    }
    if (strcmp(name, "true") == 0) return 0;
    if (strcmp(name, "upper") == 0) return upper();
    return 2;
}
