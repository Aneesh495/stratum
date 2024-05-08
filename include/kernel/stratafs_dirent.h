#ifndef STRATUM_KERNEL_STRATAFS_DIRENT_H
#define STRATUM_KERNEL_STRATAFS_DIRENT_H

#include <kernel/types.h>
#include <kernel/vfs_format.h>

#define STRATAFS_FT_UNKNOWN         0
#define STRATAFS_FT_REG             1
#define STRATAFS_FT_DIR             2

typedef struct stratafs_disk_dirent {
    uint32_t inode_num;
    uint16_t record_len;
    uint8_t  name_len;
    uint8_t  file_type;
    char     name[248];
} __attribute__((packed)) stratafs_disk_dirent_t;

static inline bool stratafs_valid_dirent(const stratafs_disk_dirent_t *entry, uint32_t inode_limit) {
    if (!entry->inode_num) return true;
    if (entry->record_len != sizeof(*entry) || entry->name_len == 0 ||
        entry->name_len >= VFS_MAX_NAME || entry->inode_num >= inode_limit ||
        (entry->file_type != STRATAFS_FT_DIR && entry->file_type != STRATAFS_FT_REG)) return false;
    for (unsigned i = 0; i < entry->name_len; i++) {
        if (!entry->name[i] || entry->name[i] == '/') return false;
    }
    return true;
}

#endif
