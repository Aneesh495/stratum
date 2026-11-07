/* Independent Linux socket client, built only for the isolated reference VM. */
#include <arpa/inet.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define SESSIONS 32U
#define LENGTH 65536U

static int transfer(int fd, void *buffer, size_t length, int outgoing) {
    uint8_t *bytes = buffer;
    while (length) {
        ssize_t n = outgoing ? send(fd, bytes, length, 0) : recv(fd, bytes, length, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        bytes += n;
        length -= (size_t)n;
    }
    return 0;
}

static uint8_t payload(uint32_t id, uint32_t offset) {
    return (uint8_t)((id * 31U + offset * 17U) ^ (offset >> 8));
}

static int session(uint32_t id) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct timeval timeout = { .tv_sec = 15 };
    struct sockaddr_in peer = { .sin_family = AF_INET, .sin_port = htons(9090) };
    uint32_t header[2] = { id, LENGTH };
    uint8_t buffer[LENGTH];
    int result = -1;
    if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) ||
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) ||
        inet_pton(AF_INET, "10.0.2.15", &peer.sin_addr) != 1 ||
        connect(fd, (struct sockaddr *)&peer, sizeof(peer))) goto done;

    for (uint32_t i = 0; i < LENGTH; i++) buffer[i] = payload(id, i);
    if (transfer(fd, header, sizeof(header), 1) || transfer(fd, buffer, sizeof(buffer), 1) ||
        shutdown(fd, SHUT_WR) || transfer(fd, buffer, sizeof(buffer), 0)) goto done;
    for (uint32_t i = 0; i < LENGTH; i++) if (buffer[i] != payload(id, i)) goto done;
    uint8_t extra;
    if (recv(fd, &extra, 1, 0) != 0) goto done;
    result = 0;
done:
    close(fd);
    return result;
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    puts("Linux TCP reference ready");
    fflush(stdout);
    if (getchar() == EOF) return 1;
    for (uint32_t id = 0; id < SESSIONS; id++) {
        if (session(id)) {
            fprintf(stderr, "Linux TCP reference failed at session %u, errno=%d\n", id, errno);
            return 1;
        }
    }
    printf("[PASS] Linux TCP reference sessions=%u bytes=%u\n", SESSIONS, SESSIONS * LENGTH * 2);
    fflush(stdout);
    for (;;) pause();
}
