#ifndef STRATUM_KERNEL_VFS_H
#define STRATUM_KERNEL_VFS_H

#include <kernel/types.h>
#include <kernel/vfs_format.h>
#include <kernel/file.h>
#include <kernel/spinlock.h>
#include <shared/syscall_nums.h>

#define VFS_APPEND_OFFSET ((uint64_t)-1)
#define VFS_MAX_PATH        256
#define VFS_MAX_MOUNTS      16

/* Standard File Open Flags */
#ifndef O_RDONLY
#define O_RDONLY    0x0000
#define O_WRONLY    0x0001
#define O_RDWR      0x0002
#define O_CREAT     0x0040
#define O_EXCL      0x0080
#define O_TRUNC     0x0200
#define O_APPEND    0x0400
#endif

/* Standard Whence for lseek */
#ifndef SEEK_SET
#define SEEK_SET    0
#define SEEK_CUR    1
#define SEEK_END    2
#endif

/* File Types */
#define VFS_TYPE_REG        0x01
#define VFS_TYPE_DIR        0x02
#define VFS_TYPE_CHR        0x03
#define VFS_TYPE_BLK        0x04
#define VFS_TYPE_FIFO       0x05
#define VFS_TYPE_SYMLINK    0x06

typedef struct vfs_stat {
    uint64_t st_ino;
    uint32_t st_mode;
    uint32_t st_nlink;
    uint32_t st_uid;
    uint32_t st_gid;
    uint64_t st_size;
    uint32_t st_blksize;
    uint64_t st_blocks;
    uint64_t st_atime;
    uint64_t st_mtime;
    uint64_t st_ctime;
} vfs_stat_t;

typedef strat_dirent_t vfs_dirent_t;

struct vfs_node;
struct vfs_mount;

typedef struct vfs_node_ops {
    int     (*open)(struct vfs_node *node, uint32_t flags);
    int     (*close)(struct vfs_node *node);
    int64_t (*read)(struct vfs_node *node, uint64_t offset, void *buf, size_t count);
    int64_t (*write)(struct vfs_node *node, uint64_t offset, const void *buf, size_t count);
    int     (*lookup)(struct vfs_node *parent, const char *name, struct vfs_node **out_child);
    int     (*create)(struct vfs_node *parent, const char *name, uint32_t mode, struct vfs_node **out_child);
    int     (*mkdir)(struct vfs_node *parent, const char *name, uint32_t mode, struct vfs_node **out_child);
    int     (*unlink)(struct vfs_node *parent, const char *name);
    int     (*rmdir)(struct vfs_node *parent, const char *name);
    int     (*link)(struct vfs_node *source, struct vfs_node *parent, const char *name);
    int     (*symlink)(struct vfs_node *parent, const char *name, const char *target);
    int64_t (*readlink)(struct vfs_node *node, char *out, size_t size);
    int     (*rename)(struct vfs_node *old_parent, const char *old_name,
                      struct vfs_node *new_parent, const char *new_name);
    int     (*readdir)(struct vfs_node *dir, uint64_t *index, vfs_dirent_t *out_dirent);
    int     (*stat)(struct vfs_node *node, vfs_stat_t *out_st);
    int     (*sync)(struct vfs_node *node);
    int     (*truncate)(struct vfs_node *node, uint64_t new_size);
} vfs_node_ops_t;

typedef struct vfs_node {
    char                    name[VFS_MAX_NAME];
    uint64_t                inode_num;
    uint32_t generation;
    uint8_t                 type;
    uint32_t                mode;
    uint32_t                uid;
    uint32_t                gid;
    uint64_t                size;
    uint64_t                atime;
    uint64_t                mtime;
    uint64_t                ctime;
    volatile uint32_t       refcount;
    struct vfs_mount       *mount;
    const vfs_node_ops_t   *ops;
    void                   *fs_priv;
    spinlock_t              lock;
} vfs_node_t;

typedef struct vfs_mount_ops {
    int (*mount)(struct vfs_mount *mnt, const char *dev);
    int (*unmount)(struct vfs_mount *mnt);
    int (*sync)(struct vfs_mount *mnt);
    int (*statfs)(struct vfs_mount *mnt, void *out_statfs);
} vfs_mount_ops_t;

typedef struct vfs_mount {
    char                    fs_name[32];
    char                    mountpoint[VFS_MAX_PATH];
    vfs_node_t             *root_node;
    void                   *dev_priv;
    void                   *fs_priv;
    const vfs_mount_ops_t  *ops;
    struct vfs_mount       *next;
    spinlock_t              lock;
} vfs_mount_t;

typedef struct vfs_filesystem {
    char                    name[32];
    const vfs_mount_ops_t  *mount_ops;
    struct vfs_filesystem  *next;
} vfs_filesystem_t;

/* Core VFS Subsystem API */
void        vfs_init(void);
int         vfs_register_fs(vfs_filesystem_t *fs);
vfs_node_t *vfs_node_alloc(void);
void        vfs_node_ref(vfs_node_t *node);
void        vfs_node_release(vfs_node_t *node);

/* Mount and Namespace Management */
int         vfs_mount(const char *dev, const char *mountpoint, const char *fs_type, uint32_t flags);
int         vfs_unmount(const char *mountpoint);
vfs_mount_t*vfs_find_mount(const char *path, const char **out_subpath);
int         vfs_resolve_path(const char *path, vfs_node_t **out_node);

/* File Operations */
int         vfs_open(const char *path, int flags, int mode, file_t **out_file);
int64_t     vfs_read(file_t *file, void *buf, size_t count);
int64_t     vfs_write(file_t *file, const void *buf, size_t count);
int64_t     vfs_lseek(file_t *file, int64_t offset, int whence);
int         vfs_close(file_t *file);
int         vfs_mkdir(const char *path, int mode);
int         vfs_unlink(const char *path);
int         vfs_rmdir(const char *path);
int         vfs_link(const char *source, const char *target);
int         vfs_symlink(const char *target, const char *name);
int64_t     vfs_readlink(const char *name, char *out, size_t size);
int         vfs_rename(const char *old_path, const char *new_path);
int         vfs_fstat(file_t *file, vfs_stat_t *st);
int         vfs_stat(const char *path, vfs_stat_t *st);
int         vfs_sync(void);

#endif /* STRATUM_KERNEL_VFS_H */
