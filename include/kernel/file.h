#ifndef STRATUM_KERNEL_FILE_H
#define STRATUM_KERNEL_FILE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <kernel/spinlock.h>
#include <kernel/mutex.h>

#define PROCESS_MAX_FDS 64

#define POLLIN      0x0001
#define POLLPRI     0x0002
#define POLLOUT     0x0004
#define POLLERR     0x0008
#define POLLHUP     0x0010
#define POLLNVAL    0x0020

struct pollfd {
    int      fd;
    int16_t  events;
    int16_t  revents;
};

typedef enum {
    FILE_TYPE_NONE = 0,
    FILE_TYPE_CONSOLE,
    FILE_TYPE_PIPE,
    FILE_TYPE_VFS,
    FILE_TYPE_SOCKET,
    FILE_TYPE_CHANNEL
} file_type_t;

struct file;

typedef struct file_ops {
    int64_t (*read)(struct file *file, void *buf, size_t count);
    int64_t (*write)(struct file *file, const void *buf, size_t count);
    int     (*poll)(struct file *file, uint32_t events);
    int     (*close)(struct file *file);
} file_ops_t;

typedef struct file {
    file_type_t       type;
    const file_ops_t *ops;
    void             *priv;
    volatile uint32_t refcount;
    uint32_t          flags;
    uint64_t          offset;
    mutex_t           lock;
} file_t;

typedef struct fd_table {
    spinlock_t lock;
    file_t    *files[PROCESS_MAX_FDS];
    uint32_t   flags[PROCESS_MAX_FDS];
    uint64_t   generation[PROCESS_MAX_FDS];
} fd_table_t;

void    file_init(void);
void    file_console_mirror(void);
uint64_t file_live_objects(void);
file_t *file_alloc(file_type_t type, const file_ops_t *ops, void *priv);
void    file_ref(file_t *f);
void    file_close(file_t *f);

file_t *file_create_console(void);

void fd_table_init(fd_table_t *table);
void fd_table_copy(fd_table_t *dst, const fd_table_t *src);
void fd_table_destroy(fd_table_t *table);
int  fd_alloc(fd_table_t *table, file_t *file);
int  fd_alloc_flags(fd_table_t *table, file_t *file, uint32_t flags);
void fd_close_exec(fd_table_t *table);
int  fd_install(fd_table_t *table, int fd, file_t *file);
file_t *fd_get(fd_table_t *table, int fd);
uint64_t fd_generation(fd_table_t *table, int fd);
file_t *fd_get_generation(fd_table_t *table, int fd, uint64_t generation);
int  fd_close(fd_table_t *table, int fd);
int  fd_dup2(fd_table_t *table, int oldfd, int newfd);

#endif /* STRATUM_KERNEL_FILE_H */
