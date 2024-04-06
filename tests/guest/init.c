#include <user/strat_api.h>
#include <shared/errno.h>

static void print(const char *s) {
    size_t len = 0;
    while (s[len]) len++;
    write(1, s, len);
}

static int strncmp(const char *s1, const char *s2, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (s1[i] != s2[i]) return (int)((unsigned char)s1[i] - (unsigned char)s2[i]);
        if (s1[i] == '\0') break;
    }
    return 0;
}

static volatile int g_cow_test_var = 111;

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    print("[USER] Stratum native userland program running in Ring 3!\n");
    print("[USER] Testing syscalls from Ring 3: getpid, uptime, nanosleep...\n");

    int my_pid = getpid();
    if (my_pid != 1) {
        print("[USER] ERROR: Init process PID is not 1!\n");
        return 1;
    }

    uint64_t up = uptime();
    (void)up;
    print("[USER] System call getpid and uptime succeeded!\n");

    /* 1. Pipe creation test */
    print("[USER] Creating anonymous IPC pipe...\n");
    int pfd[2];
    int pipe_res = pipe(pfd);
    if (pipe_res != 0) {
        print("[USER] ERROR: pipe() syscall failed!\n");
        return 2;
    }
    print("[USER] Anonymous pipe created successfully.\n");

    /* 2. Process fork test */
    print("[USER] Forking child process...\n");
    int cpid = fork();
    if (cpid < 0) {
        print("[USER] ERROR: fork() syscall failed!\n");
        return 3;
    }

    if (cpid == 0) {
        /* Child Process */
        print("[CHILD] Child process entered Ring 3!\n");
        int child_pid = getpid();
        if (child_pid == 1) {
            print("[CHILD] ERROR: Child PID cannot be 1!\n");
            exit(99);
        }

        /* Test Copy-On-Write write fault */
        g_cow_test_var = 999;
        print("[CHILD] 1. COW write done\n");

        /* Close read end */
        close(pfd[0]);
        print("[CHILD] 2. close read end done\n");

        /* Write data into pipe */
        const char *child_msg = "stratum_pipe_ipc_message";
        write(pfd[1], child_msg, 24);
        print("[CHILD] 3. write to pipe done\n");

        /* Close write end */
        close(pfd[1]);
        print("[CHILD] 4. close write end done\n");

        sleep_ms(15);
        print("[CHILD] Child process completed IPC write, exiting with status 77.\n");
        exit(77);
    }

    /* Parent Process */
    /* Close write end */
    close(pfd[1]);

    /* Verify COW isolation: parent's copy must remain 111 */
    if (g_cow_test_var != 111) {
        print("[USER] ERROR: Copy-On-Write isolation broken in parent!\n");
        return 4;
    }
    print("[USER] Copy-On-Write memory isolation verified in parent.\n");

    /* 3. Test poll() on pipe read end */
    print("[USER] Testing poll() on pipe read descriptor...\n");
    struct pollfd fds_to_poll;
    fds_to_poll.fd = pfd[0];
    fds_to_poll.events = POLLIN;
    fds_to_poll.revents = 0;

    int poll_res = poll(&fds_to_poll, 1, 1000);
    if (poll_res <= 0 || !(fds_to_poll.revents & POLLIN)) {
        print("[USER] ERROR: poll() did not indicate POLLIN readiness!\n");
        return 5;
    }
    print("[USER] poll() indicated POLLIN readiness successfully.\n");

    /* 4. Read from pipe */
    print("[USER] About to read from pipe...\n");
    char buf[64];
    for (int i = 0; i < 64; i++) buf[i] = 0;
    int bytes_read = read(pfd[0], buf, sizeof(buf) - 1);
    if (bytes_read <= 0 || strncmp(buf, "stratum_pipe_ipc_message", 24) != 0) {
        print("[USER] ERROR: Pipe read returned: ");
        if (bytes_read <= 0) {
            print(bytes_read == 0 ? "EOF 0\n" : "negative error\n");
        } else {
            print("content '");
            print(buf);
            print("'\n");
        }
        return 6;
    }
    print("[USER] Read message from child via pipe: stratum_pipe_ipc_message\n");

    /* 5. Verify EOF on closed pipe */
    char eof_check[16];
    int eof_bytes = read(pfd[0], eof_check, sizeof(eof_check));
    if (eof_bytes != 0) {
        print("[USER] ERROR: Closed pipe did not return EOF 0!\n");
        return 7;
    }
    print("[USER] Pipe EOF detection verified upon writer close.\n");
    close(pfd[0]);

    /* 6. Test waitpid() */
    print("[USER] Waiting for child process via waitpid()...\n");
    int child_status = 0;
    int reaped = waitpid(cpid, &child_status, 0);
    if (reaped != cpid || child_status != 77) {
        print("[USER] ERROR: waitpid() failed or returned incorrect status!\n");
        return 8;
    }
    print("[USER] Child process reaped successfully with status 77.\n");

    /* 7. Test dup2() */
    print("[USER] Testing dup2() descriptor duplication...\n");
    int dup_pipe[2];
    pipe(dup_pipe);
    int new_fd = 10;
    int dup_res = dup2(dup_pipe[1], new_fd);
    if (dup_res != new_fd) {
        print("[USER] ERROR: dup2() failed!\n");
        return 9;
    }
    write(new_fd, "dup_ok", 6);
    char dup_buf[16];
    read(dup_pipe[0], dup_buf, 6);
    close(dup_pipe[0]);
    close(dup_pipe[1]);
    close(new_fd);
    print("[USER] dup2() verified successfully.\n");

    for (int cycle = 0; cycle < 64; cycle++) {
        int child = fork();
        if (child < 0) return 10;
        if (child == 0) {
            g_cow_test_var = cycle;
            if (cycle == 63) __asm__ volatile("ud2");
            exit(cycle);
        }
        int status = -1;
        if (cycle == 63 &&
            waitpid(child, (int *)0xFFFFFFFF80000000ULL, 0) != -STRATUM_EFAULT) return 11;
        if (waitpid(child, &status, 0) != child || status != (cycle == 63 ? 134 : cycle)) return 12;
        if (g_cow_test_var != 111) return 13;
    }
    if (syscall1(SYS_thread_self, 0) != -STRATUM_ENOSYS) return 14;
    print("[USER] 64 fork/reap cycles, COW, invalid status pointer and Ring 3 fault isolation verified.\n");
    const size_t length = 128 * 4096;
    int64_t mapped = mmap_anonymous(length, PROT_READ | PROT_WRITE);
    if (mapped < 0) return 15;
    volatile char *pages = (volatile char *)mapped;
    for (size_t page = 0; page < length; page += 4096) {
        if (pages[page] != 0) return 16;
        pages[page] = 'a';
    }
    if (mprotect((void *)pages, length, PROT_READ) != 0) return 17;
    int writer = fork();
    if (writer < 0) return 18;
    if (writer == 0) {
        if (mprotect((void *)pages, length, PROT_READ | PROT_WRITE) != 0) exit(99);
        pages[0] = 'b';
        exit(pages[0] == 'b' ? 0 : 99);
    }
    int status;
    if (waitpid(writer, &status, 0) != writer || status != 0 || pages[0] != 'a') return 19;

    writer = fork();
    if (writer < 0) return 20;
    if (writer == 0) {
        pages[0] = 'c';
        exit(99);
    }
    if (waitpid(writer, &status, 0) != writer || status != 142) return 21;
    if (munmap((void *)pages, length) != 0 || munmap((void *)pages, length) != -STRATUM_EINVAL) return 22;
    print("[USER] 128 demand-zero pages, mapping protections, and anonymous COW verified.\n");
    print("[USER] ABI and IPC smoke checks complete.\n");
    return 42;
}
