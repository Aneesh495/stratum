#include <kernel/vfs.h>
#include <kernel/pmm.h>
#include <kernel/string.h>
#include <shared/errno.h>

#define CPIO_HEADER 110
#define INITRAMFS_FILES 64

typedef struct {
    const char *name;
    const uint8_t *data;
    uint32_t size;
    uint32_t mode;
} archive_file_t;

static archive_file_t files[INITRAMFS_FILES];
static unsigned file_count;
static const boot_handoff_t *archive_boot;
static bool mounted;

static bool hex_field(const uint8_t *field, uint32_t *out) {
    uint32_t value = 0;
    for (unsigned i = 0; i < 8; i++) {
        uint8_t c = field[i];
        if (c >= '0' && c <= '9') value = (value << 4) | (c - '0');
        else if (c >= 'a' && c <= 'f') value = (value << 4) | (c - 'a' + 10);
        else return false;
    }
    *out = value;
    return true;
}

static int scan_archive(const uint8_t *archive, size_t size) {
    file_count = 0;
    size_t cursor = 0;
    while (cursor <= size && size - cursor >= CPIO_HEADER) {
        const uint8_t *header = archive + cursor;
        uint32_t mode, data_size, name_size;
        if (memcmp(header, "070701", 6) || !hex_field(header + 14, &mode) ||
            !hex_field(header + 54, &data_size) || !hex_field(header + 94, &name_size)) return -STRATUM_EINVAL;
        cursor += CPIO_HEADER;
        if (!name_size || name_size > VFS_MAX_NAME + 4 || name_size > size - cursor) return -STRATUM_EINVAL;
        const char *name = (const char *)(archive + cursor);
        for (uint32_t i = 0; i + 1 < name_size; i++) if (!name[i]) return -STRATUM_EINVAL;
        if (name[name_size - 1]) return -STRATUM_EINVAL;
        cursor = round_up(cursor + name_size, 4);
        if (cursor > size || data_size > size - cursor) return -STRATUM_EINVAL;
        if (strcmp(name, "TRAILER!!!") == 0) {
            if (data_size || !file_count) return -STRATUM_EINVAL;
            for (; cursor < size; cursor++) if (archive[cursor]) return -STRATUM_EINVAL;
            return 0;
        }
        if ((mode & VFS_MODE_IFMT) == VFS_MODE_IFDIR) {
            if (data_size || (strcmp(name, ".") && strcmp(name, "bin"))) return -STRATUM_EINVAL;
        } else {
            if ((mode & VFS_MODE_IFMT) != VFS_MODE_IFREG || strncmp(name, "bin/", 4) ||
                !name[4] || strchr(name + 4, '/') || !strcmp(name + 4, ".") ||
                !strcmp(name + 4, "..") || file_count == INITRAMFS_FILES) return -STRATUM_EINVAL;
            for (unsigned i = 0; i < file_count; i++) if (!strcmp(files[i].name, name + 4)) return -STRATUM_EINVAL;
            files[file_count++] = (archive_file_t){name + 4, archive + cursor, data_size, mode & ~0222U};
        }
        cursor = round_up(cursor + data_size, 4);
    }
    return -STRATUM_EINVAL;
}

static int archive_open(vfs_node_t *node, uint32_t flags) {
    if (flags & (3U | O_TRUNC | O_APPEND)) return -STRATUM_EROFS;
    if (node->type == VFS_TYPE_DIR && !(flags & O_DIRECTORY)) return -STRATUM_EISDIR;
    return 0;
}

static int64_t archive_read(vfs_node_t *node, uint64_t offset, void *buf, size_t count) {
    const archive_file_t *file = node->fs_priv;
    if (!file) return -STRATUM_EISDIR;
    if (offset >= file->size) return 0;
    if (count > file->size - offset) count = file->size - offset;
    memcpy(buf, file->data + offset, count);
    return (int64_t)count;
}

static const vfs_node_ops_t archive_file_ops = {
    .open = archive_open,
    .read = archive_read,
};

static int archive_lookup(vfs_node_t *dir, const char *name, vfs_node_t **out) {
    if (!strcmp(name, ".") || !strcmp(name, "..")) {
        vfs_node_ref(dir);
        *out = dir;
        return 0;
    }
    for (unsigned i = 0; i < file_count; i++) {
        if (strcmp(name, files[i].name)) continue;
        vfs_node_t *node = vfs_node_alloc();
        if (!node) return -STRATUM_ENOMEM;
        strcpy(node->name, name);
        node->inode_num = i + 2;
        node->type = VFS_TYPE_REG;
        node->mode = files[i].mode;
        node->size = files[i].size;
        node->mount = dir->mount;
        node->fs_priv = &files[i];
        node->ops = &archive_file_ops;
        *out = node;
        return 0;
    }
    return -STRATUM_ENOENT;
}

static int archive_readdir(vfs_node_t *node, uint64_t *index, vfs_dirent_t *entry) {
    (void)node;
    if (*index >= file_count) return 0;
    entry->d_ino = *index + 2;
    entry->d_type = VFS_TYPE_REG;
    strcpy(entry->d_name, files[*index].name);
    (*index)++;
    return 1;
}

static const vfs_node_ops_t archive_dir_ops = {
    .open = archive_open,
    .lookup = archive_lookup,
    .readdir = archive_readdir,
};

static int archive_mount(vfs_mount_t *mount, const char *dev) {
    (void)dev;
    if (mounted) return -STRATUM_EBUSY;
    if (!archive_boot->initramfs_size) return -STRATUM_ENOENT;
    int result = scan_archive(phys_to_virt(archive_boot->initramfs_phys), archive_boot->initramfs_size);
    if (result) return result;
    vfs_node_t *root = vfs_node_alloc();
    if (!root) return -STRATUM_ENOMEM;
    strcpy(root->name, "bin");
    root->inode_num = 1;
    root->type = VFS_TYPE_DIR;
    root->mode = VFS_MODE_IFDIR | 0555;
    root->mount = mount;
    root->ops = &archive_dir_ops;
    mount->root_node = root;
    mounted = true;
    return 0;
}

static const vfs_mount_ops_t archive_mount_ops = { .mount = archive_mount };
static vfs_filesystem_t archive_fs = { .name = "initramfs", .mount_ops = &archive_mount_ops };

int initramfs_mount_bin(const boot_handoff_t *handoff) {
    archive_boot = handoff;
    int result = vfs_register_fs(&archive_fs);
    return result ? result : vfs_mount("boot archive", "/bin", "initramfs", 0);
}
