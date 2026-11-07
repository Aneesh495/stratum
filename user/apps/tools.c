#include <user/strat_api.h>
#include <shared/sha256.h>

extern int strcmp(const char *, const char *);
extern void u_dprintf(int, const char *, ...);

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

static int copy_file(const char *source, const char *destination) {
    int in = open(source, O_RDONLY, 0);
    if (in < 0) return 1;
    int out = open(destination, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (out < 0) {
        close(in);
        return 1;
    }

    char bytes[512];
    int64_t amount;
    int result = 0;
    while ((amount = read(in, bytes, sizeof(bytes))) > 0) {
        if (write(out, bytes, (size_t)amount) != amount) {
            result = 1;
            break;
        }
    }
    if (amount < 0 || fsync(out)) result = 1;
    if (close(out)) result = 1;
    if (close(in)) result = 1;
    if (result) unlink(destination);
    return result;
}

static int checksum(const char *path) {
    int fd = path ? open(path, O_RDONLY, 0) : 0;
    if (fd < 0) return 1;
    sha256_ctx_t context;
    sha256_init(&context);
    char buffer[512];
    int64_t count;
    while ((count = read(fd, buffer, sizeof(buffer))) > 0) sha256_update(&context, buffer, (size_t)count);
    if (path) close(fd);
    if (count < 0) return 1;

    uint8_t digest[SHA256_DIGEST_SIZE];
    const char hex[] = "0123456789abcdef";
    char text[2 * SHA256_DIGEST_SIZE + 1];
    sha256_final(&context, digest);
    for (unsigned i = 0; i < SHA256_DIGEST_SIZE; i++) {
        text[2 * i] = hex[digest[i] >> 4];
        text[2 * i + 1] = hex[digest[i] & 15];
    }
    text[sizeof(text) - 1] = '\n';
    return write(1, text, sizeof(text)) != sizeof(text);
}

int main(int argc, char **argv) {
    if (!argc || !argv[0]) return 2;
    const char *name = argv[0];
    for (const char *p = name; *p; p++) {
        if (*p == '/') name = p + 1;
    }
    if (strcmp(name, "true") == 0) return 0;
    if (strcmp(name, "upper") == 0) return upper();
    if (strcmp(name, "sha256sum") == 0 && argc <= 2) return checksum(argc == 2 ? argv[1] : NULL);
    if (strcmp(name, "cp") == 0 && argc == 3) {
        int result = copy_file(argv[1], argv[2]);
        if (result) u_dprintf(2, "cp: unable to copy %s to %s\n", argv[1], argv[2]);
        return result;
    }
    return 2;
}
