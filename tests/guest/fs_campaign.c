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

int main(void) {
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
