#include <kernel/vfs.h>
#include <kernel/slab.h>
#include <kernel/kernel.h>
#include <kernel/string.h>
#include <shared/errno.h>

static vfs_mount_t      *g_mounts = NULL;
static vfs_filesystem_t *g_filesystems = NULL;
static spinlock_t        g_vfs_lock;

static int64_t vfs_file_read(file_t *file, void *buf, size_t count) {
    if (!file || !file->priv || !buf) return -STRATUM_EINVAL;
    if ((file->flags & 3U) == O_WRONLY) return -STRATUM_EBADF;
    if (file->flags & O_DIRECTORY) return -STRATUM_EISDIR;
    vfs_node_t *node = (vfs_node_t *)file->priv;
    if (!node->ops || !node->ops->read) return -STRATUM_ENOSYS;

    uint64_t flags;
    spin_lock_irqsave(&file->lock, &flags);
    int64_t bytes = node->ops->read(node, file->offset, buf, count);
    if (bytes > 0) {
        file->offset += (uint64_t)bytes;
    }
    spin_unlock_irqrestore(&file->lock, flags);
    return bytes;
}

static int64_t vfs_file_write(file_t *file, const void *buf, size_t count) {
    if (!file || !file->priv || !buf) return -STRATUM_EINVAL;
    if ((file->flags & 3U) == O_RDONLY) return -STRATUM_EBADF;
    if (count == 0) return 0;
    vfs_node_t *node = (vfs_node_t *)file->priv;
    if (!node->ops || !node->ops->write) return -STRATUM_ENOSYS;

    uint64_t flags;
    spin_lock_irqsave(&file->lock, &flags);
    bool append = (file->flags & O_APPEND) != 0;
    int64_t bytes = node->ops->write(node, append ? VFS_APPEND_OFFSET : file->offset, buf, count);
    if (bytes > 0) {
        file->offset = append ? node->size : file->offset + (uint64_t)bytes;
    }
    spin_unlock_irqrestore(&file->lock, flags);
    return bytes;
}

static int vfs_file_poll(file_t *file, uint32_t events) {
    (void)file;
    int revents = 0;
    if (events & (POLLIN | POLLOUT)) {
        revents |= (events & (POLLIN | POLLOUT));
    }
    return revents;
}

static int vfs_file_close(file_t *file) {
    if (!file || !file->priv) return 0;
    vfs_node_t *node = (vfs_node_t *)file->priv;
    if (node->ops && node->ops->close) {
        node->ops->close(node);
    }
    vfs_node_release(node);
    file->priv = NULL;
    return 0;
}

static const file_ops_t g_vfs_file_ops = {
    .read  = vfs_file_read,
    .write = vfs_file_write,
    .poll  = vfs_file_poll,
    .close = vfs_file_close,
};

void vfs_init(void) {
    spin_lock_init(&g_vfs_lock);
    g_mounts = NULL;
    g_filesystems = NULL;
    kprintf("[VFS] Virtual File System initialized.\n");
}

int vfs_register_fs(vfs_filesystem_t *fs) {
    if (!fs) return -STRATUM_EINVAL;

    uint64_t flags;
    spin_lock_irqsave(&g_vfs_lock, &flags);
    fs->next = g_filesystems;
    g_filesystems = fs;
    spin_unlock_irqrestore(&g_vfs_lock, flags);

    kprintf("[VFS] Registered filesystem: %s\n", fs->name);
    return 0;
}

vfs_node_t *vfs_node_alloc(void) {
    vfs_node_t *node = (vfs_node_t *)kmalloc(sizeof(vfs_node_t));
    if (!node) return NULL;
    memset(node, 0, sizeof(vfs_node_t));
    node->refcount = 1;
    spin_lock_init(&node->lock);
    return node;
}

void vfs_node_ref(vfs_node_t *node) {
    if (!node) return;
    __atomic_add_fetch(&node->refcount, 1, __ATOMIC_SEQ_CST);
}

void vfs_node_release(vfs_node_t *node) {
    if (!node) return;
    if (__atomic_sub_fetch(&node->refcount, 1, __ATOMIC_SEQ_CST) == 0) {
        kfree(node);
    }
}

int vfs_mount(const char *dev, const char *mountpoint, const char *fs_type, uint32_t flags) {
    (void)flags;
    if (!mountpoint || !fs_type) return -STRATUM_EINVAL;

    uint64_t lock_flags;
    spin_lock_irqsave(&g_vfs_lock, &lock_flags);

    vfs_filesystem_t *fs = g_filesystems;
    while (fs) {
        if (strcmp(fs->name, fs_type) == 0) break;
        fs = fs->next;
    }

    if (!fs) {
        spin_unlock_irqrestore(&g_vfs_lock, lock_flags);
        kprintf("[VFS] Filesystem '%s' not registered!\n", fs_type);
        return -STRATUM_ENODEV;
    }

    vfs_mount_t *mnt = (vfs_mount_t *)kmalloc(sizeof(vfs_mount_t));
    if (!mnt) {
        spin_unlock_irqrestore(&g_vfs_lock, lock_flags);
        return -STRATUM_ENOMEM;
    }

    memset(mnt, 0, sizeof(vfs_mount_t));
    strncpy(mnt->fs_name, fs_type, sizeof(mnt->fs_name) - 1);
    strncpy(mnt->mountpoint, mountpoint, sizeof(mnt->mountpoint) - 1);
    mnt->ops = fs->mount_ops;
    spin_lock_init(&mnt->lock);

    int res = mnt->ops->mount(mnt, dev);
    if (res != 0) {
        kfree(mnt);
        spin_unlock_irqrestore(&g_vfs_lock, lock_flags);
        return res;
    }

    /* Insert into mount list (head insertion: newer mounts match first) */
    mnt->next = g_mounts;
    g_mounts = mnt;

    spin_unlock_irqrestore(&g_vfs_lock, lock_flags);
    kprintf("[VFS] Mounted %s at '%s' (type %s)\n", dev ? dev : "none", mountpoint, fs_type);
    return 0;
}

int vfs_unmount(const char *mountpoint) {
    if (!mountpoint) return -STRATUM_EINVAL;

    uint64_t flags;
    spin_lock_irqsave(&g_vfs_lock, &flags);

    vfs_mount_t *prev = NULL;
    vfs_mount_t *cur = g_mounts;

    while (cur) {
        if (strcmp(cur->mountpoint, mountpoint) == 0) {
            if (prev) prev->next = cur->next;
            else g_mounts = cur->next;
            break;
        }
        prev = cur;
        cur = cur->next;
    }

    spin_unlock_irqrestore(&g_vfs_lock, flags);

    if (!cur) return -STRATUM_ENOENT;

    if (cur->ops && cur->ops->unmount) {
        cur->ops->unmount(cur);
    }
    if (cur->root_node) {
        vfs_node_release(cur->root_node);
    }
    kfree(cur);
    return 0;
}

vfs_mount_t *vfs_find_mount(const char *path, const char **out_subpath) {
    if (!path || path[0] != '/') return NULL;

    vfs_mount_t *best_mnt = NULL;
    size_t best_len = 0;

    for (vfs_mount_t *m = g_mounts; m != NULL; m = m->next) {
        size_t mlen = strlen(m->mountpoint);
        if (mlen == 1 && m->mountpoint[0] == '/') {
            if (best_len == 0) {
                best_mnt = m;
                best_len = 1;
            }
        } else if (strncmp(path, m->mountpoint, mlen) == 0) {
            if (path[mlen] == '/' || path[mlen] == '\0') {
                if (mlen > best_len) {
                    best_mnt = m;
                    best_len = mlen;
                }
            }
        }
    }

    if (best_mnt && out_subpath) {
        const char *sub = path + best_len;
        while (*sub == '/') sub++;
        *out_subpath = sub;
    }

    return best_mnt;
}

static int split_component(const char **path_ptr, char *out_name) {
    const char *p = *path_ptr;
    while (*p == '/') p++;
    if (*p == '\0') return 0;

    size_t len = 0;
    while (*p != '/' && *p != '\0') {
        if (len == VFS_MAX_NAME - 1) return -STRATUM_ENAMETOOLONG;
        out_name[len++] = *p++;
    }
    out_name[len] = '\0';
    *path_ptr = p;
    return 1;
}

int vfs_resolve_path(const char *path, vfs_node_t **out_node) {
    if (!path || !out_node) return -STRATUM_EINVAL;

    const char *subpath = NULL;
    vfs_mount_t *mnt = vfs_find_mount(path, &subpath);
    if (!mnt || !mnt->root_node) return -STRATUM_ENOENT;

    vfs_node_t *curr = mnt->root_node;
    vfs_node_ref(curr);

    char comp[VFS_MAX_NAME];
    int component;
    while ((component = split_component(&subpath, comp)) > 0) {

        if (!curr->ops || !curr->ops->lookup) {
            vfs_node_release(curr);
            return -STRATUM_ENOTDIR;
        }

        vfs_node_t *next = NULL;
        int res = curr->ops->lookup(curr, comp, &next);
        vfs_node_release(curr);
        if (res != 0 || !next) {
            return res ? res : -STRATUM_EIO;
        }
        curr = next;
    }

    if (component < 0) {
        vfs_node_release(curr);
        return component;
    }
    *out_node = curr;
    return 0;
}

static int vfs_resolve_parent(const char *path, vfs_node_t **out_parent, char *out_name) {
    if (!path || !out_parent || !out_name) return -STRATUM_EINVAL;

    const char *subpath = NULL;
    vfs_mount_t *mnt = vfs_find_mount(path, &subpath);
    if (!mnt || !mnt->root_node) return -STRATUM_ENOENT;

    vfs_node_t *curr = mnt->root_node;
    vfs_node_ref(curr);

    char comp[VFS_MAX_NAME];
    int component;
    while ((component = split_component(&subpath, comp)) > 0) {
        const char *lookahead = subpath;
        while (*lookahead == '/') lookahead++;
        if (*lookahead == '\0') {
            if (strcmp(comp, ".") == 0 || strcmp(comp, "..") == 0) {
                vfs_node_release(curr);
                return -STRATUM_EINVAL;
            }
            strcpy(out_name, comp);
            *out_parent = curr;
            return 0;
        }

        /* Not the leaf, descend */
        if (!curr->ops || !curr->ops->lookup) {
            vfs_node_release(curr);
            return -STRATUM_ENOTDIR;
        }

        vfs_node_t *next = NULL;
        int res = curr->ops->lookup(curr, comp, &next);
        vfs_node_release(curr);
        if (res != 0 || !next) {
            return res ? res : -STRATUM_EIO;
        }
        curr = next;
    }

    vfs_node_release(curr);
    return component < 0 ? component : -STRATUM_EINVAL;
}

int vfs_open(const char *path, int flags, int mode, file_t **out_file) {
    if (!path || !out_file || (flags & 3) == 3 ||
        ((flags & O_TRUNC) && (flags & 3) == O_RDONLY) ||
        ((flags & O_DIRECTORY) && (flags & (3 | O_CREAT | O_TRUNC | O_APPEND)))) return -STRATUM_EINVAL;

    vfs_node_t *node = NULL;
    int res = vfs_resolve_path(path, &node);

    if (res != 0) {
        if (res == -STRATUM_ENOENT && (flags & O_CREAT)) {
            vfs_node_t *parent = NULL;
            char name[VFS_MAX_NAME];
            res = vfs_resolve_parent(path, &parent, name);
            if (res != 0) return res;

            if (!parent->ops || !parent->ops->create) {
                vfs_node_release(parent);
                return -STRATUM_ENOSYS;
            }

            res = parent->ops->create(parent, name, (uint32_t)mode, &node);
            vfs_node_release(parent);
            if (res == -STRATUM_EEXIST && !(flags & O_EXCL)) {
                res = vfs_resolve_path(path, &node);
            }
            if (res != 0) return res;
        } else {
            return res;
        }
    } else if ((flags & (O_CREAT | O_EXCL)) == (O_CREAT | O_EXCL)) {
        vfs_node_release(node);
        return -STRATUM_EEXIST;
    }

    if ((flags & O_DIRECTORY) && node->type != VFS_TYPE_DIR) {
        vfs_node_release(node);
        return -STRATUM_ENOTDIR;
    }

    if (node->ops && node->ops->open) {
        int o_res = node->ops->open(node, (uint32_t)flags);
        if (o_res != 0) {
            vfs_node_release(node);
            return o_res;
        }
    }

    file_t *f = file_alloc(FILE_TYPE_VFS, &g_vfs_file_ops, node);
    if (!f) {
        if (node->ops && node->ops->close) node->ops->close(node);
        vfs_node_release(node);
        return -STRATUM_ENOMEM;
    }

    f->flags = (uint32_t)flags;
    f->offset = 0;
    if (flags & O_APPEND) {
        f->offset = node->size;
    }

    *out_file = f;
    return 0;
}

int64_t vfs_read(file_t *file, void *buf, size_t count) {
    if (!file || !file->ops || !file->ops->read) return -STRATUM_EBADF;
    return file->ops->read(file, buf, count);
}

int64_t vfs_write(file_t *file, const void *buf, size_t count) {
    if (!file || !file->ops || !file->ops->write) return -STRATUM_EBADF;
    return file->ops->write(file, buf, count);
}

int64_t vfs_lseek(file_t *file, int64_t offset, int whence) {
    if (!file || file->type != FILE_TYPE_VFS || !file->priv) return -STRATUM_EBADF;
    vfs_node_t *node = (vfs_node_t *)file->priv;
    if (file->flags & O_DIRECTORY) return -STRATUM_ESPIPE;

    uint64_t flags;
    spin_lock_irqsave(&file->lock, &flags);

    int64_t base = 0;
    int res = 0;
    switch (whence) {
    case SEEK_SET:
        break;
    case SEEK_CUR:
        base = (int64_t)file->offset;
        break;
    case SEEK_END: {
        vfs_stat_t st;
        res = node->ops && node->ops->stat ? node->ops->stat(node, &st) : -STRATUM_ENOSYS;
        if (res == 0) base = (int64_t)st.st_size;
        break;
    }
    default:
        res = -STRATUM_EINVAL;
        break;
    }

    int64_t new_off;
    if (res == 0 && (__builtin_add_overflow(base, offset, &new_off) || new_off < 0)) {
        res = -STRATUM_EINVAL;
    }
    if (res != 0) {
        spin_unlock_irqrestore(&file->lock, flags);
        return res;
    }

    file->offset = (uint64_t)new_off;
    spin_unlock_irqrestore(&file->lock, flags);
    return new_off;
}

int vfs_close(file_t *file) {
    if (!file) return -STRATUM_EBADF;
    file_close(file);
    return 0;
}

int vfs_mkdir(const char *path, int mode) {
    if (!path) return -STRATUM_EINVAL;

    vfs_node_t *parent = NULL;
    char name[VFS_MAX_NAME];
    int res = vfs_resolve_parent(path, &parent, name);
    if (res != 0) return res;

    if (!parent->ops || !parent->ops->mkdir) {
        vfs_node_release(parent);
        return -STRATUM_ENOSYS;
    }

    vfs_node_t *child = NULL;
    res = parent->ops->mkdir(parent, name, (uint32_t)mode, &child);
    if (child) {
        vfs_node_release(child);
    }
    vfs_node_release(parent);
    return res;
}

int vfs_unlink(const char *path) {
    if (!path) return -STRATUM_EINVAL;

    vfs_node_t *parent = NULL;
    char name[VFS_MAX_NAME];
    int res = vfs_resolve_parent(path, &parent, name);
    if (res != 0) return res;

    if (!parent->ops || !parent->ops->unlink) {
        vfs_node_release(parent);
        return -STRATUM_ENOSYS;
    }

    res = parent->ops->unlink(parent, name);
    vfs_node_release(parent);
    return res;
}

int vfs_rename(const char *old_path, const char *new_path) {
    vfs_node_t *old_parent, *new_parent;
    char old_name[VFS_MAX_NAME], new_name[VFS_MAX_NAME];
    int res = vfs_resolve_parent(old_path, &old_parent, old_name);
    if (res != 0) return res;
    res = vfs_resolve_parent(new_path, &new_parent, new_name);
    if (res == 0) {
        if (old_parent->mount != new_parent->mount) res = -STRATUM_EXDEV;
        else if (!old_parent->ops || !old_parent->ops->rename) res = -STRATUM_ENOTSUP;
        else res = old_parent->ops->rename(old_parent, old_name, new_parent, new_name);
        vfs_node_release(new_parent);
    }
    vfs_node_release(old_parent);
    return res;
}

int vfs_stat(const char *path, vfs_stat_t *st) {
    if (!path || !st) return -STRATUM_EINVAL;

    vfs_node_t *node = NULL;
    int res = vfs_resolve_path(path, &node);
    if (res != 0) return res;

    if (node->ops && node->ops->stat) {
        res = node->ops->stat(node, st);
    } else {
        memset(st, 0, sizeof(vfs_stat_t));
        st->st_ino = node->inode_num;
        st->st_mode = node->mode;
        st->st_size = node->size;
        st->st_nlink = 1;
        st->st_blksize = 4096;
        st->st_blocks = (node->size + 511) / 512;
        st->st_atime = node->atime;
        st->st_mtime = node->mtime;
        st->st_ctime = node->ctime;
        res = 0;
    }

    vfs_node_release(node);
    return res;
}

int vfs_sync(void) {
    uint64_t flags;
    spin_lock_irqsave(&g_vfs_lock, &flags);

    int res = 0;
    for (vfs_mount_t *m = g_mounts; m != NULL && res == 0; m = m->next) {
        if (m->ops && m->ops->sync) res = m->ops->sync(m);
    }

    spin_unlock_irqrestore(&g_vfs_lock, flags);
    return res;
}
