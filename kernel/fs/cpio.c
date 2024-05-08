#include <kernel/cpio.h>
#include <kernel/vfs_format.h>
#include <kernel/string.h>
#include <shared/errno.h>

#define CPIO_HEADER 110

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

int cpio_scan_bin(const uint8_t *archive, size_t size, cpio_file_t *files, unsigned capacity,
                  unsigned *file_count) {
    if (!archive || !files || !file_count) return -STRATUM_EINVAL;
    *file_count = 0;
    unsigned count = 0;
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
            if (data_size || !count) return -STRATUM_EINVAL;
            for (; cursor < size; cursor++) if (archive[cursor]) return -STRATUM_EINVAL;
            *file_count = count;
            return 0;
        }
        if ((mode & VFS_MODE_IFMT) == VFS_MODE_IFDIR) {
            if (data_size || (strcmp(name, ".") && strcmp(name, "bin"))) return -STRATUM_EINVAL;
        } else {
            if ((mode & VFS_MODE_IFMT) != VFS_MODE_IFREG || strncmp(name, "bin/", 4) ||
                !name[4] || strchr(name + 4, '/') || !strcmp(name + 4, ".") ||
                !strcmp(name + 4, "..") || count == capacity) return -STRATUM_EINVAL;
            for (unsigned i = 0; i < count; i++) if (!strcmp(files[i].name, name + 4)) return -STRATUM_EINVAL;
            files[count++] = (cpio_file_t){name + 4, archive + cursor, data_size, mode & ~0222U};
        }
        cursor = round_up(cursor + data_size, 4);
    }
    return -STRATUM_EINVAL;
}
