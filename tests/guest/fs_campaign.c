#include <stdbool.h>
#include <shared/errno.h>
#include <user/strat_api.h>

extern void u_printf(const char *, ...);
extern int strcmp(const char *, const char *);

#define ROUNDS 4000
#define SOURCE "/from/source"
#define TARGET "/to/target"

static unsigned operations;

#define CHECK(operation) do { \
    if (!(operation)) { \
        u_printf("fs-campaign: operation failed at source line %u, completed=%u\n", __LINE__, operations); \
        return 1; \
    } \
    operations++; \
} while (0)

static int exercise(unsigned round) {
    uint8_t first[512], tail[31], received[512];
    for (unsigned i = 0; i < sizeof(first); i++) first[i] = (uint8_t)(round * 43 + i * 7);
    for (unsigned i = 0; i < sizeof(tail); i++) tail[i] = (uint8_t)(round * 97 + i * 13);
    int64_t tail_offset = (round % 5 ? 4096 : 12 * 4096) + (round % 97);
    user_stat_t st;
    int fd;

    CHECK((fd = open(SOURCE, O_CREAT | O_EXCL | O_RDWR, 0644)) >= 0);
    CHECK(write(fd, first, sizeof(first)) == sizeof(first));
    CHECK(lseek(fd, tail_offset, SEEK_SET) == tail_offset);
    CHECK(write(fd, tail, sizeof(tail)) == sizeof(tail));
    CHECK(fsync(fd) == 0);
    CHECK(stat(SOURCE, &st) == 0 && st.st_size == (uint64_t)tail_offset + sizeof(tail));
    uint64_t source_ino = st.st_ino;
    CHECK(lseek(fd, 0, SEEK_SET) == 0);
    CHECK(read(fd, received, sizeof(received)) == sizeof(received));
    for (unsigned i = 0; i < sizeof(first); i++) if (received[i] != first[i]) return 1;
    CHECK(lseek(fd, 1024, SEEK_SET) == 1024);
    CHECK(read(fd, received, sizeof(received)) == sizeof(received));
    for (unsigned i = 0; i < sizeof(received); i++) if (received[i]) return 1;
    CHECK(lseek(fd, tail_offset, SEEK_SET) == tail_offset);
    CHECK(read(fd, received, sizeof(tail)) == sizeof(tail));
    for (unsigned i = 0; i < sizeof(tail); i++) if (received[i] != tail[i]) return 1;
    CHECK(close(fd) == 0);

    CHECK((fd = open(TARGET, O_CREAT | O_EXCL | O_WRONLY, 0644)) >= 0);
    CHECK(write(fd, "replaced", 8) == 8);
    CHECK(close(fd) == 0);
    CHECK(rename(SOURCE, TARGET) == 0);
    CHECK(stat(TARGET, &st) == 0 && st.st_ino == source_ino && st.st_size == (uint64_t)tail_offset + sizeof(tail));
    if (stat(SOURCE, &st) != -STRATUM_ENOENT) return 1;
    CHECK((fd = open(TARGET, O_RDONLY, 0)) >= 0);
    CHECK(lseek(fd, tail_offset, SEEK_SET) == tail_offset);
    CHECK(read(fd, received, sizeof(tail)) == sizeof(tail));
    for (unsigned i = 0; i < sizeof(tail); i++) if (received[i] != tail[i]) return 1;
    CHECK(close(fd) == 0);

    CHECK((fd = open("/to", O_RDONLY | O_DIRECTORY, 0)) >= 0);
    strat_dirent_t entry;
    unsigned matches = 0;
    for (;;) {
        int n = readdir(fd, &entry);
        CHECK(n >= 0);
        if (!n) break;
        if (!strcmp(entry.d_name, "target") && entry.d_ino == source_ino) matches++;
    }
    if (matches != 1) return 1;
    CHECK(close(fd) == 0);
    CHECK((fd = open(TARGET, O_WRONLY | O_TRUNC, 0)) >= 0);
    CHECK(close(fd) == 0);
    CHECK(stat(TARGET, &st) == 0 && !st.st_size && st.st_ino == source_ino);
    CHECK(unlink(TARGET) == 0);
    return 0;
}

static int check_namespace(void) {
    user_stat_t left, right, branch, parent, payload, alias;
    CHECK(stat("/left", &left) == 0 && left.st_nlink == 2);
    CHECK(stat("/right", &right) == 0 && right.st_nlink == 3);
    CHECK(stat("/right/branch", &branch) == 0 && branch.st_nlink == 3);
    CHECK(stat("/right/branch/..", &parent) == 0 && parent.st_ino == right.st_ino);
    CHECK(stat("/right/branch/child/..", &parent) == 0 && parent.st_ino == branch.st_ino);
    CHECK(stat("/left/tree", &parent) == -STRATUM_ENOENT);
    CHECK(stat("/right/branch/data", &payload) == 0 && payload.st_nlink == 2);
    CHECK(stat("/left/alias", &alias) == 0 && alias.st_ino == payload.st_ino && alias.st_nlink == 2);
    int fd;
    char bytes[9] = {0};
    char target[32] = {0};
    CHECK(readlink("/left/jump", target, sizeof(target)) == 18 && !strcmp(target, "/right/branch/data"));
    CHECK(stat("/left/shortcut/data", &alias) == 0 && alias.st_ino == payload.st_ino);
    CHECK(stat("/left/a", &alias) == -STRATUM_ELOOP);
    CHECK((fd = open("/left/jump", O_RDONLY, 0)) >= 0);
    CHECK(read(fd, bytes, 8) == 8 && !strcmp(bytes, "survives"));
    CHECK(close(fd) == 0);
    return 0;
}

static int setup_namespace(void) {
    int fd;
    CHECK(mkdir("/left", 0755) == 0);
    CHECK(mkdir("/right", 0755) == 0);
    CHECK(mkdir("/left/tree", 0755) == 0);
    CHECK(mkdir("/left/tree/child", 0755) == 0);
    CHECK(mkdir("/right/branch", 0755) == 0);
    CHECK((fd = open("/left/tree/data", O_CREAT | O_EXCL | O_RDWR, 0644)) >= 0);
    CHECK(write(fd, "survives", 8) == 8);
    CHECK(fsync(fd) == 0);
    CHECK(link("/left/tree/data", "/left/alias") == 0);
    CHECK(link("/left/tree/data", "/left/alias") == -STRATUM_EEXIST);
    CHECK(link("/left/tree", "/left/directory-alias") == -STRATUM_EPERM);
    CHECK(link("/left/tree/data", "/bin/link") == -STRATUM_EXDEV);
    CHECK(unlink("/left/tree") == -STRATUM_EISDIR);
    CHECK(rmdir("/left/tree") == -STRATUM_ENOTEMPTY);
    CHECK(rmdir("/left/tree/data") == -STRATUM_ENOTDIR);
    CHECK(rename("/left/tree", "/left/tree/child/cycle") == -STRATUM_EINVAL);
    CHECK(rename("/left/tree", "/right/branch") == 0);
    CHECK(rename("/right/branch/data", "/left/alias") == 0);
    CHECK(lseek(fd, 0, SEEK_SET) == 0);
    char bytes[9] = {0};
    CHECK(read(fd, bytes, 8) == 8 && !strcmp(bytes, "survives"));
    CHECK(close(fd) == 0);
    CHECK(mkdir("/right/reject", 0755) == 0);
    CHECK(rename("/right/reject", "/right/branch") == -STRATUM_ENOTEMPTY);
    CHECK(rmdir("/right/reject") == 0);
    CHECK(symlink("/right/branch/data", "/left/jump") == 0);
    CHECK(symlink("/right/branch", "/left/shortcut") == 0);
    CHECK(symlink("/left/b", "/left/a") == 0);
    CHECK(symlink("/left/a", "/left/b") == 0);
    CHECK(link("/left/jump", "/left/another") == -STRATUM_EPERM);
    CHECK(open("/left/a", O_CREAT | O_EXCL | O_RDWR, 0600) == -STRATUM_EEXIST);
    return check_namespace();
}

static int cleanup_namespace(void) {
    user_stat_t file;
    if (check_namespace()) return 1;
    CHECK(unlink("/left/jump") == 0);
    CHECK(unlink("/left/shortcut") == 0);
    CHECK(unlink("/left/a") == 0);
    CHECK(unlink("/left/b") == 0);
    CHECK(unlink("/left/alias") == 0);
    CHECK(stat("/right/branch/data", &file) == 0 && file.st_nlink == 1);
    CHECK(unlink("/right/branch/data") == 0);
    CHECK(rmdir("/right/branch/child") == 0);
    CHECK(rmdir("/right/branch") == 0);
    CHECK(rmdir("/left") == 0);
    CHECK(rmdir("/right") == 0);
    return 0;
}

static int open_unlinked(bool crash) {
    const char *path = "/orphan";
    int file, replacement;
    user_stat_t previous, next;
    char payload[5] = {0};
    CHECK((file = open(path, O_CREAT | O_EXCL | O_RDWR, 0600)) >= 0);
    CHECK(write(file, "held", 4) == 4);
    CHECK(stat(path, &previous) == 0);
    CHECK(dup2(file, 10) == 10);
    CHECK(unlink(path) == 0);
    CHECK(stat(path, &next) == -STRATUM_ENOENT);
    CHECK((replacement = open(path, O_CREAT | O_EXCL | O_WRONLY, 0600)) >= 0);
    CHECK(write(replacement, "new", 3) == 3);
    CHECK(stat(path, &next) == 0 && previous.st_ino != next.st_ino);
    CHECK(close(replacement) == 0);
    CHECK(unlink(path) == 0);
    CHECK(close(file) == 0);
    CHECK(lseek(10, 0, SEEK_SET) == 0);
    CHECK(read(10, payload, 4) == 4 && !strcmp(payload, "held"));
    if (crash) {
        u_printf("[READY] orphan survived unlink with last descriptor open\n");
        for (;;) sleep_ms(1000);
    }
    CHECK(close(10) == 0);
    CHECK((replacement = open(path, O_CREAT | O_EXCL | O_WRONLY, 0600)) >= 0);
    CHECK(stat(path, &next) == 0 && previous.st_ino == next.st_ino);
    CHECK(close(replacement) == 0);
    CHECK(unlink(path) == 0);
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "namespace")) {
        if (setup_namespace()) return 1;
        u_printf("[PASS] StrataFS directory rename and hard links committed\n");
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "reopen")) {
        if (cleanup_namespace()) return 1;
        u_printf("[PASS] StrataFS namespace survived reboot and was reclaimed\n");
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "orphan")) {
        if (open_unlinked(false)) return 1;
        u_printf("[PASS] final close reclaimed the unlinked inode\n");
        return open_unlinked(true);
    }
    if (argc != 1) return 1;
    CHECK(mkdir("/from", 0755) == 0);
    CHECK(mkdir("/to", 0755) == 0);
    for (unsigned round = 0; round < ROUNDS; round++) {
        if (exercise(round)) {
            u_printf("fs-campaign: failed at round=%u\n", round);
            return 1;
        }
        if ((round + 1) % 1000 == 0) u_printf("fs-campaign: rounds=%u operations=%u\n", round + 1, operations);
    }
    int fd;
    CHECK((fd = open("/done", O_CREAT | O_EXCL | O_WRONLY, 0644)) >= 0);
    CHECK(write(fd, "stratum-storage-verified", 24) == 24);
    CHECK(fsync(fd) == 0);
    CHECK(close(fd) == 0);
    u_printf("[PASS] StrataFS mixed operations=%u rounds=%u durable fixture saved\n", operations, ROUNDS);
    return 0;
}
