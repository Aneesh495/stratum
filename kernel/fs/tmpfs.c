#include <kernel/tmpfs.h>
#include <kernel/mutex.h>
#include <kernel/pmm.h>
#include <kernel/string.h>
#include <kernel/vfs.h>
#include <shared/errno.h>

#define INODES 128U
#define FILE_PAGES 16U
#define VOLUME_PAGES 256U
#define DETACHED ((uint32_t)-1)

typedef struct {
    uint32_t generation;
    uint32_t parent;
    uint32_t mode;
    unsigned open;
    uint64_t size;
    uint64_t pages[FILE_PAGES];
    char name[VFS_MAX_NAME];
} tmp_inode_t;

static struct {
    mutex_t lock;
    unsigned pages;
    vfs_mount_t *mount;
    tmp_inode_t inodes[INODES];
} tmp;

static const vfs_node_ops_t node_ops;

static bool directory(const tmp_inode_t *inode) {
    return (inode->mode & VFS_MODE_IFMT) == VFS_MODE_IFDIR;
}

static tmp_inode_t *checked(const vfs_node_t *node) {
    if (node->inode_num >= INODES) return NULL;
    tmp_inode_t *inode = &tmp.inodes[node->inode_num];
    return inode->mode && inode->generation == node->generation ? inode : NULL;
}

static vfs_node_t *make_node(unsigned index) {
    const tmp_inode_t *inode = &tmp.inodes[index];
    vfs_node_t *node = vfs_node_alloc();
    if (!node) return NULL;
    node->inode_num = index;
    node->generation = inode->generation;
    node->size = inode->size;
    node->type = directory(inode) ? VFS_TYPE_DIR : VFS_TYPE_REG;
    node->mode = inode->mode;
    node->mount = tmp.mount;
    node->ops = &node_ops;
    strcpy(node->name, inode->name);
    return node;
}

static int child(unsigned parent, const char *name) {
    for (unsigned i = 1; i < INODES; i++) {
        if (tmp.inodes[i].mode && tmp.inodes[i].parent == parent && !strcmp(tmp.inodes[i].name, name)) {
            return (int)i;
        }
    }
    return -STRATUM_ENOENT;
}

static int lookup(vfs_node_t *parent, const char *name, vfs_node_t **out) {
    mutex_lock(&tmp.lock);
    tmp_inode_t *inode = checked(parent);
    int index = -STRATUM_ENOENT;
    if (inode) {
        if (!directory(inode)) index = -STRATUM_ENOTDIR;
        else if (!strcmp(name, ".")) index = (int)parent->inode_num;
        else if (!strcmp(name, "..")) index = (int)inode->parent;
        else index = child((unsigned)parent->inode_num, name);
    }
    int result = index;
    if (index >= 0) {
        *out = make_node((unsigned)index);
        result = *out ? 0 : -STRATUM_ENOMEM;
    }
    mutex_unlock(&tmp.lock);
    return result;
}

static int create_node(vfs_node_t *parent, const char *name, uint32_t mode, vfs_node_t **out) {
    mutex_lock(&tmp.lock);
    tmp_inode_t *dir = checked(parent);
    int result = 0;
    if (!dir) result = -STRATUM_ENOENT;
    else if (!directory(dir)) result = -STRATUM_ENOTDIR;
    else if (child((unsigned)parent->inode_num, name) >= 0) result = -STRATUM_EEXIST;
    else {
        unsigned i = 1;
        while (i < INODES && tmp.inodes[i].mode) i++;
        if (i == INODES) result = -STRATUM_ENOSPC;
        else {
            tmp_inode_t *inode = &tmp.inodes[i];
            if (!++inode->generation) inode->generation++;
            inode->parent = (unsigned)parent->inode_num;
            inode->mode = mode;
            strcpy(inode->name, name);
            *out = make_node(i);
            if (!*out) {
                inode->mode = 0;
                result = -STRATUM_ENOMEM;
            }
        }
    }
    mutex_unlock(&tmp.lock);
    return result;
}

static int create(vfs_node_t *parent, const char *name, uint32_t mode, vfs_node_t **out) {
    return create_node(parent, name, (mode & 0777) | VFS_MODE_IFREG, out);
}

static int make_directory(vfs_node_t *parent, const char *name, uint32_t mode, vfs_node_t **out) {
    return create_node(parent, name, (mode & 0777) | VFS_MODE_IFDIR, out);
}

static void free_pages(tmp_inode_t *inode) {
    for (unsigned i = 0; i < FILE_PAGES; i++) {
        if (!inode->pages[i]) continue;
        pmm_page_release(inode->pages[i]);
        inode->pages[i] = 0;
        tmp.pages--;
    }
    inode->size = 0;
}

static bool empty(unsigned parent) {
    for (unsigned i = 1; i < INODES; i++) {
        if (tmp.inodes[i].mode && tmp.inodes[i].parent == parent) return false;
    }
    return true;
}

static void detach(tmp_inode_t *inode) {
    inode->parent = DETACHED;
    if (!inode->open) {
        free_pages(inode);
        inode->mode = 0;
    }
}

static int removable(unsigned index, bool dir) {
    tmp_inode_t *inode = &tmp.inodes[index];
    if (directory(inode) != dir) return dir ? -STRATUM_ENOTDIR : -STRATUM_EISDIR;
    if (dir && !empty(index)) return -STRATUM_ENOTEMPTY;
    if (dir && inode->open) return -STRATUM_EBUSY;
    return 0;
}

static int remove_node(vfs_node_t *parent, const char *name, bool dir) {
    mutex_lock(&tmp.lock);
    tmp_inode_t *inode = checked(parent);
    int index = !inode ? -STRATUM_ENOENT : (!directory(inode) ? -STRATUM_ENOTDIR :
                                                child((unsigned)parent->inode_num, name));
    int result = index < 0 ? index : removable((unsigned)index, dir);
    if (!result) detach(&tmp.inodes[index]);
    mutex_unlock(&tmp.lock);
    return result;
}

static int unlink_node(vfs_node_t *parent, const char *name) {
    return remove_node(parent, name, false);
}

static int remove_directory(vfs_node_t *parent, const char *name) {
    return remove_node(parent, name, true);
}

static int rename_node(vfs_node_t *from, const char *old, vfs_node_t *to, const char *name) {
    mutex_lock(&tmp.lock);
    tmp_inode_t *source_dir = checked(from), *target_dir = checked(to);
    int result = 0;
    if (!source_dir || !target_dir) result = -STRATUM_ENOENT;
    else if (!directory(source_dir) || !directory(target_dir)) result = -STRATUM_ENOTDIR;
    else {
        int source = child((unsigned)from->inode_num, old);
        int target = child((unsigned)to->inode_num, name);
        if (source < 0) result = source;
        else if (source != target) {
            tmp_inode_t *inode = &tmp.inodes[source];
            if (directory(inode)) {
                for (unsigned parent = (unsigned)to->inode_num; parent; parent = tmp.inodes[parent].parent) {
                    if (parent == (unsigned)source) {
                        result = -STRATUM_EINVAL;
                        break;
                    }
                }
            }
            if (!result && target >= 0) result = removable((unsigned)target, directory(inode));
            if (!result) {
                if (target >= 0) detach(&tmp.inodes[target]);
                inode->parent = (unsigned)to->inode_num;
                strcpy(inode->name, name);
            }
        }
    }
    mutex_unlock(&tmp.lock);
    return result;
}

static int open_node(vfs_node_t *node, uint32_t flags) {
    mutex_lock(&tmp.lock);
    tmp_inode_t *inode = checked(node);
    int result = 0;
    if (!inode || inode->parent == DETACHED) result = -STRATUM_ENOENT;
    else if (directory(inode) && !(flags & O_DIRECTORY)) result = -STRATUM_EISDIR;
    else {
        if (flags & O_TRUNC) free_pages(inode);
        inode->open++;
        node->size = inode->size;
    }
    mutex_unlock(&tmp.lock);
    return result;
}

static int close_node(vfs_node_t *node) {
    mutex_lock(&tmp.lock);
    tmp_inode_t *inode = checked(node);
    if (inode && inode->open && !--inode->open && inode->parent == DETACHED) detach(inode);
    mutex_unlock(&tmp.lock);
    return 0;
}

static int64_t read_node(vfs_node_t *node, uint64_t offset, void *out, size_t length) {
    mutex_lock(&tmp.lock);
    const tmp_inode_t *inode = checked(node);
    int64_t result = -STRATUM_ENOENT;
    if (inode) {
        uint64_t available = offset < inode->size ? inode->size - offset : 0;
        size_t size = length < available ? length : (size_t)available;
        for (size_t done = 0; done < size;) {
            uint64_t position = offset + done;
            size_t start = position % PAGE_SIZE;
            size_t capacity = PAGE_SIZE - start;
            size_t amount = size - done < capacity ? size - done : capacity;
            uint64_t page = inode->pages[position / PAGE_SIZE];
            if (page) memcpy((uint8_t *)out + done, (uint8_t *)phys_to_virt(page) + start, amount);
            else memset((uint8_t *)out + done, 0, amount);
            done += amount;
        }
        result = (int64_t)size;
    }
    mutex_unlock(&tmp.lock);
    return result;
}

static int64_t write_node(vfs_node_t *node, uint64_t offset, const void *in, size_t length) {
    mutex_lock(&tmp.lock);
    tmp_inode_t *inode = checked(node);
    int64_t result = -STRATUM_ENOENT;
    if (!inode) goto out;
    if (offset == VFS_APPEND_OFFSET) offset = inode->size;
    if (offset > FILE_PAGES * PAGE_SIZE || length > FILE_PAGES * PAGE_SIZE - offset) {
        result = -STRATUM_EFBIG;
        goto out;
    }
    size_t done = 0;
    while (done < length) {
        uint64_t position = offset + done;
        size_t start = position % PAGE_SIZE;
        size_t capacity = PAGE_SIZE - start;
        size_t amount = length - done < capacity ? length - done : capacity;
        uint64_t *page = &inode->pages[position / PAGE_SIZE];
        if (!*page) {
            if (tmp.pages == VOLUME_PAGES || !(*page = pmm_alloc_page())) break;
            tmp.pages++;
        }
        memcpy((uint8_t *)phys_to_virt(*page) + start, (const uint8_t *)in + done, amount);
        done += amount;
    }
    if (done && offset + done > inode->size) inode->size = offset + done;
    node->size = inode->size;
    result = done || !length ? (int64_t)done : -STRATUM_ENOSPC;
out:
    mutex_unlock(&tmp.lock);
    return result;
}

static int readdir_node(vfs_node_t *node, uint64_t *index, vfs_dirent_t *entry) {
    mutex_lock(&tmp.lock);
    const tmp_inode_t *inode = checked(node);
    int result = 0;
    if (!inode) result = -STRATUM_ENOENT;
    else if (!directory(inode)) result = -STRATUM_ENOTDIR;
    else if (*index < 2) {
        strcpy(entry->d_name, *index ? ".." : ".");
        entry->d_ino = *index ? inode->parent : node->inode_num;
        entry->d_type = VFS_TYPE_DIR;
        (*index)++;
        result = 1;
    } else {
        while (*index < INODES + 2) {
            uint64_t child_index = (*index)++ - 2;
            const tmp_inode_t *child_inode = &tmp.inodes[child_index];
            if (!child_index || !child_inode->mode || child_inode->parent != node->inode_num) continue;
            strcpy(entry->d_name, child_inode->name);
            entry->d_ino = child_index;
            entry->d_type = directory(child_inode) ? VFS_TYPE_DIR : VFS_TYPE_REG;
            result = 1;
            break;
        }
    }
    mutex_unlock(&tmp.lock);
    return result;
}

static int stat_node(vfs_node_t *node, vfs_stat_t *out) {
    mutex_lock(&tmp.lock);
    const tmp_inode_t *inode = checked(node);
    if (inode) {
        memset(out, 0, sizeof(*out));
        out->st_ino = node->inode_num;
        out->st_mode = inode->mode;
        out->st_size = inode->size;
        out->st_blksize = PAGE_SIZE;
        out->st_nlink = inode->parent == DETACHED ? 0 : 1;
        if (directory(inode)) {
            out->st_nlink = 2;
            for (unsigned i = 1; i < INODES; i++) {
                if (tmp.inodes[i].parent == node->inode_num && directory(&tmp.inodes[i])) out->st_nlink++;
            }
        }
        for (unsigned i = 0; i < FILE_PAGES; i++) if (inode->pages[i]) out->st_blocks += PAGE_SIZE / 512;
    }
    mutex_unlock(&tmp.lock);
    return inode ? 0 : -STRATUM_ENOENT;
}

static int sync_node(vfs_node_t *node) {
    (void)node;
    return 0;
}

static const vfs_node_ops_t node_ops = {
    .open = open_node, .close = close_node, .read = read_node, .write = write_node,
    .lookup = lookup, .create = create, .mkdir = make_directory,
    .unlink = unlink_node, .rmdir = remove_directory, .rename = rename_node,
    .readdir = readdir_node, .stat = stat_node, .sync = sync_node,
};

static int mount_volume(vfs_mount_t *mount, const char *device) {
    (void)device;
    if (tmp.mount) return -STRATUM_EBUSY;
    mutex_init(&tmp.lock);
    tmp.mount = mount;
    tmp.inodes[0] = (tmp_inode_t){ .generation = 1, .mode = VFS_MODE_IFDIR | 0777, .name = "/" };
    mount->root_node = make_node(0);
    if (!mount->root_node) {
        tmp.mount = NULL;
        return -STRATUM_ENOMEM;
    }
    return 0;
}

static const vfs_mount_ops_t mount_ops = { .mount = mount_volume };
static vfs_filesystem_t filesystem = { .name = "tmpfs", .mount_ops = &mount_ops };

int tmpfs_init(void) {
    return vfs_register_fs(&filesystem);
}
