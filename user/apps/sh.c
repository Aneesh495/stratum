#include <user/strat_api.h>

extern void u_printf(const char *fmt, ...);
extern int ledgerd_run(void);
extern int ledger_put(const char *id, const char *value);
extern int ledger_pull(const char *peer, const char *id);
extern size_t strlen(const char *s);
extern char *strncpy(char *dest, const char *src, size_t n);
extern int strcmp(const char *s1, const char *s2);

static void cmd_help(void) {
    u_printf("Stratum Native Interactive Shell (v0.1.0)\n");
    u_printf("Supported Commands:\n");
    u_printf("  help                Display available shell commands\n");
    u_printf("  echo [args...]      Display text arguments to stdout\n");
    u_printf("  cat <path>          Print contents of a file\n");
    u_printf("  ls [path]           List directory entries\n");
    u_printf("  mv <from> <to>      Rename or replace a regular file\n");
    u_printf("  mkdir <path>        Create a directory in the filesystem\n");
    u_printf("  rm <path>           Unlink / delete a file\n");
    u_printf("  write <path> <text> Append text and sync it to disk\n");
    u_printf("  stat <path>         Display file metadata and size\n");
    u_printf("  ledgerput <id> <value>  Store a local durable record\n");
    u_printf("  ledgerpull <ip> <id>    Fetch and store a peer record\n");
    u_printf("  ledgerd             Serve durable records over TCP\n");
    u_printf("  sysinfo             Display kernel memory, CPUs, and uptime\n");
}

static void cmd_echo(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        u_printf("%s%s", argv[i], (i + 1 < argc) ? " " : "");
    }
    u_printf("\n");
}

static void cmd_cat(const char *path) {
    if (!path) {
        u_printf("Usage: cat <file>\n");
        return;
    }
    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) {
        u_printf("cat: failed to open '%s' (err=%d)\n", path, fd);
        return;
    }
    char buf[256];
    int64_t r;
    while ((r = read(fd, buf, sizeof(buf))) > 0) {
        write(1, buf, (size_t)r);
    }
    if (r < 0) u_printf("cat: read failed (err=%d)\n", (int)r);
    close(fd);
}

static int cmd_ls(const char *path) {
    int fd = open(path, O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0) {
        u_printf("ls: cannot open '%s' (err=%d)\n", path, fd);
        return fd;
    }
    strat_dirent_t entry;
    int res;
    while ((res = readdir(fd, &entry)) > 0) {
        u_printf("%s%s\n", entry.d_name, entry.d_type == 2 ? "/" : "");
    }
    close(fd);
    if (res < 0) u_printf("ls: cannot read '%s' (err=%d)\n", path, res);
    return res;
}

static void cmd_stat(const char *path) {
    if (!path) {
        u_printf("Usage: stat <file>\n");
        return;
    }
    user_stat_t st;
    int res = stat(path, &st);
    if (res != 0) {
        u_printf("stat: cannot stat '%s' (err=%d)\n", path, res);
        return;
    }
    u_printf("  File: %s\n", path);
    u_printf("  Size: %lu bytes    Inode: %lu    Mode: 0x%x\n",
             st.st_size, st.st_ino, st.st_mode);
}

static void cmd_mkdir(const char *path) {
    if (!path) {
        u_printf("Usage: mkdir <path>\n");
        return;
    }
    int res = mkdir(path, 0755);
    if (res != 0) {
        u_printf("mkdir: failed to create directory '%s' (err=%d)\n", path, res);
    } else {
        u_printf("Directory '%s' created.\n", path);
    }
}

static void cmd_rm(const char *path) {
    if (!path) {
        u_printf("Usage: rm <path>\n");
        return;
    }
    int res = unlink(path);
    if (res != 0) {
        u_printf("rm: failed to unlink '%s' (err=%d)\n", path, res);
    } else {
        u_printf("File '%s' unlinked.\n", path);
    }
}

static void cmd_sysinfo(void) {
    strat_sysinfo_t info;
    int res = sysinfo(&info);
    if (res != 0) {
        u_printf("sysinfo failed (err=%d)\n", res);
        return;
    }
    u_printf("System Information:\n");
    u_printf("  Uptime:       %lu ms\n", info.uptime_ms);
    u_printf("  Online CPUs:  %u\n", info.online_cpus);
    u_printf("  Processes:    %u\n", info.active_processes);
    u_printf("  Total Memory: %lu KiB\n", info.total_memory_bytes / 1024);
    u_printf("  Free Memory:  %lu KiB\n", info.free_memory_bytes / 1024);
    u_printf("  Syscalls:     %lu\n", info.total_syscalls);
}

static int cmd_write(int argc, char **argv) {
    if (argc < 3) {
        u_printf("Usage: write <file> <text>\n");
        return -1;
    }
    int fd = open(argv[1], O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) {
        u_printf("write: failed to open '%s' (err=%d)\n", argv[1], fd);
        return fd;
    }
    char text[128];
    size_t len = 0;
    for (int i = 2; i < argc; i++) {
        if (i > 2) text[len++] = ' ';
        for (const char *p = argv[i]; *p; p++) text[len++] = *p;
    }
    text[len++] = '\n';
    int64_t written = write(fd, text, len);
    int result = written == (int64_t)len ? fsync(fd) : -1;
    close(fd);
    if (result != 0) u_printf("write: failed to sync '%s' (err=%d)\n", argv[1], result);
    return result;
}

int sh_execute_cmd(const char *cmd_line) {
    if (!cmd_line || !cmd_line[0]) return 0;

    char buf[128];
    strncpy(buf, cmd_line, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *argv[8];
    int argc = 0;
    char *p = buf;

    while (*p && argc < 8) {
        while (*p == ' ') p++;
        if (!*p) break;
        argv[argc++] = p;
        while (*p && *p != ' ') p++;
        if (*p) {
            *p++ = '\0';
        }
    }

    while (*p == ' ') p++;
    if (*p) {
        u_printf("sh: too many arguments\n");
        return -1;
    }
    if (argc == 0) return 0;

    if (strcmp(argv[0], "help") == 0) {
        cmd_help();
    } else if (strcmp(argv[0], "echo") == 0) {
        cmd_echo(argc, argv);
    } else if (strcmp(argv[0], "cat") == 0) {
        cmd_cat(argc > 1 ? argv[1] : NULL);
    } else if (strcmp(argv[0], "ls") == 0) {
        return cmd_ls(argc > 1 ? argv[1] : "/");
    } else if (strcmp(argv[0], "mv") == 0) {
        if (argc != 3) {
            u_printf("Usage: mv <from> <to>\n");
            return -1;
        }
        int res = rename(argv[1], argv[2]);
        if (res != 0) u_printf("mv: failed (err=%d)\n", res);
        return res;
    } else if (strcmp(argv[0], "stat") == 0) {
        cmd_stat(argc > 1 ? argv[1] : NULL);
    } else if (strcmp(argv[0], "mkdir") == 0) {
        cmd_mkdir(argc > 1 ? argv[1] : NULL);
    } else if (strcmp(argv[0], "rm") == 0) {
        cmd_rm(argc > 1 ? argv[1] : NULL);
    } else if (strcmp(argv[0], "sysinfo") == 0) {
        cmd_sysinfo();
    } else if ((strcmp(argv[0], "ledgerput") == 0 || strcmp(argv[0], "ledgerpull") == 0) && argc == 3) {
        int res = strcmp(argv[0], "ledgerput") == 0 ? ledger_put(argv[1], argv[2]) :
                                                     ledger_pull(argv[1], argv[2]);
        if (res == 0) u_printf("Ledger record durable.\n");
        else u_printf("Ledger operation failed (err=%d)\n", res);
        return res;
    } else if (strcmp(argv[0], "ledgerd") == 0) {
        int res = ledgerd_run();
        if (res < 0) u_printf("ledgerd: failed (err=%d)\n", res);
        return res;
    } else if (strcmp(argv[0], "write") == 0) {
        return cmd_write(argc, argv);
    } else {
        u_printf("sh: command not found: '%s'\n", argv[0]);
        return -1;
    }

    return 0;
}
