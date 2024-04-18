#include <shared/errno.h>
#include <stdbool.h>
#include <user/strat_api.h>

extern void u_printf(const char *fmt, ...);
extern void u_dprintf(int fd, const char *fmt, ...);
extern int ledgerd_run(void);
extern int ledger_put(const char *id, const char *value);
extern int ledger_pull(const char *peer, const char *id);
extern size_t strlen(const char *s);
extern void *memcpy(void *dest, const void *src, size_t n);
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

#define SH_MAX_STAGES 4
#define SH_MAX_ARGS 8
#define SH_LINE_CAPACITY 128
#define SH_EXTERNAL (-1000)

typedef struct {
    char *argv[SH_MAX_ARGS + 1];
    int argc;
    char *input;
    char *output;
    int output_flags;
} sh_stage_t;

static int sh_builtin(int argc, char **argv) {
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
        return SH_EXTERNAL;
    }

    return 0;
}

static bool is_separator(char ch) {
    return !ch || ch == ' ' || ch == '\t' || ch == '<' || ch == '>' || ch == '|';
}

static int parse_pipeline(const char *line, char *words, sh_stage_t *stages) {
    int count = 1;
    sh_stage_t *stage = &stages[0];
    char **redirect = NULL;
    while (*line) {
        if (*line == ' ' || *line == '\t') {
            line++;
            continue;
        }
        if (*line == '|') {
            if (redirect || !stage->argc || count == SH_MAX_STAGES) return -1;
            stage = &stages[count++];
            line++;
            continue;
        }
        if (*line == '<' || *line == '>') {
            if (redirect) return -1;
            if (*line++ == '<') {
                redirect = &stage->input;
            } else {
                redirect = &stage->output;
                stage->output_flags = O_WRONLY | O_CREAT | O_TRUNC;
                if (*line == '>') {
                    line++;
                    stage->output_flags = O_WRONLY | O_CREAT | O_APPEND;
                }
            }
            if (*redirect) return -1;
            continue;
        }
        char *word = words;
        char quote = 0;
        while (*line && (quote || !is_separator(*line))) {
            char ch = *line++;
            if (ch == '\\' && quote != '\'') {
                if (!*line) return -1;
                *words++ = *line++;
            } else if (quote == ch) {
                quote = 0;
            } else if (!quote && (ch == '\'' || ch == '"')) {
                quote = ch;
            } else {
                *words++ = ch;
            }
        }
        if (quote) return -1;
        *words++ = 0;
        if (redirect) {
            *redirect = word;
            redirect = NULL;
        } else {
            if (stage->argc == SH_MAX_ARGS) return -1;
            stage->argv[stage->argc++] = word;
        }
    }
    if (redirect) return -1;
    if (!stage->argc) return count == 1 && !stage->input && !stage->output ? 0 : -1;
    return count;
}

static int redirect_fd(const char *path, int flags, int dest) {
    if (!path) return 0;
    int fd = open(path, flags, 0644);
    if (fd < 0) {
        u_dprintf(2, "sh: cannot open '%s' (err=%d)\n", path, fd);
        return fd;
    }
    if (fd == dest) return 0;
    int result = dup2(fd, dest);
    close(fd);
    return result;
}

static void close_pipes(int pipes[][2], int count) {
    for (int i = 0; i < count; i++) {
        close(pipes[i][0]);
        close(pipes[i][1]);
    }
}

static void run_stage(sh_stage_t *stage, int pipes[][2], int count, int i) {
    if ((i > 0 && dup2(pipes[i - 1][0], 0) < 0) ||
        (i < count - 1 && dup2(pipes[i][1], 1) < 0)) exit(1);
    close_pipes(pipes, count - 1);
    if (redirect_fd(stage->input, O_RDONLY, 0) < 0 ||
        redirect_fd(stage->output, stage->output_flags, 1) < 0) exit(1);
    int status = sh_builtin(stage->argc, stage->argv);
    if (status != SH_EXTERNAL) exit(status < 0 ? 1 : status);

    const char *command = stage->argv[0];
    char path[SH_LINE_CAPACITY + sizeof("/bin/")];
    if (command[0] != '/') {
        memcpy(path, "/bin/", sizeof("/bin/") - 1);
        memcpy(path + sizeof("/bin/") - 1, command, strlen(command) + 1);
        command = path;
    }
    int error = execve(command, (const char *const *)stage->argv, NULL);
    u_dprintf(2, "sh: cannot execute '%s' (err=%d)\n", command, error);
    exit(error == -STRATUM_ENOENT ? 127 : 126);
}

int sh_execute_cmd(const char *line) {
    if (!line || strlen(line) >= SH_LINE_CAPACITY) return -1;
    char words[SH_LINE_CAPACITY];
    sh_stage_t stages[SH_MAX_STAGES] = {0};
    int count = parse_pipeline(line, words, stages);
    if (count < 0) {
        u_dprintf(2, "sh: invalid command syntax\n");
        return -1;
    }
    if (count == 0) return 0;
    if (count == 1 && !stages[0].input && !stages[0].output) {
        int status = sh_builtin(stages[0].argc, stages[0].argv);
        if (status != SH_EXTERNAL) return status;
    }

    int pipes[SH_MAX_STAGES - 1][2];
    for (int i = 0; i < count - 1; i++) {
        if (pipe(pipes[i]) < 0) {
            close_pipes(pipes, i);
            u_dprintf(2, "sh: cannot create pipeline\n");
            return 1;
        }
    }
    int pids[SH_MAX_STAGES];
    int launched = 0;
    for (; launched < count; launched++) {
        int pid = fork();
        if (pid == 0) run_stage(&stages[launched], pipes, count, launched);
        if (pid < 0) {
            u_dprintf(2, "sh: cannot start process (err=%d)\n", pid);
            break;
        }
        pids[launched] = pid;
    }
    close_pipes(pipes, count - 1);
    int result = launched == count ? 0 : 1;
    for (int i = 0; i < launched; i++) {
        int status;
        if (waitpid(pids[i], &status, 0) != pids[i]) {
            result = 1;
        } else if (i == count - 1 && !result) {
            result = status;
        }
    }
    return result;
}
