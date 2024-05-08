#include <kernel/stratafs.h>
#include <kernel/slab.h>
#include <kernel/kernel.h>
#include <kernel/string.h>
#include <kernel/sched.h>
#include <shared/errno.h>

#define STRATAFS_MAX_FILE_SIZE ((STRATAFS_NDIR_BLOCKS + STRATAFS_INDIR_BLOCKS) * STRATAFS_BLOCK_SIZE)
#define STRATAFS_MAX_BITMAP_BITS (STRATAFS_BLOCK_SIZE * 8U)
#define STRATAFS_MODE_MASK 0170000

_Static_assert(sizeof(stratafs_sb_t) == STRATAFS_BLOCK_SIZE, "StrataFS superblock layout");
_Static_assert(sizeof(stratafs_disk_inode_t) == STRATAFS_INODE_SIZE, "StrataFS inode stride");
_Static_assert(sizeof(stratafs_disk_dirent_t) == 256, "StrataFS directory entry layout");

static stratafs_t g_stratafs;
static bool       g_stratafs_active = false;

static vfs_node_t *stratafs_create_vfs_node(stratafs_t *fs, uint32_t ino, const stratafs_disk_inode_t *dinode, const char *name);
extern const vfs_node_ops_t g_stratafs_node_ops;

int stratafs_read_block(stratafs_t *fs, uint64_t block_num, void *buf) {
    if (!fs || !fs->dev || !buf) return -STRATUM_EINVAL;
    if (block_num >= fs->dev->capacity_sectors / STRATAFS_SECTORS_PER_BLOCK) return -STRATUM_EIO;
    uint64_t sector = block_num * STRATAFS_SECTORS_PER_BLOCK;
    int64_t rd = virtio_blk_read(fs->dev, sector, STRATAFS_SECTORS_PER_BLOCK, buf);
    return (rd == STRATAFS_BLOCK_SIZE) ? 0 : -STRATUM_EIO;
}

int stratafs_write_block(stratafs_t *fs, uint64_t block_num, const void *buf) {
    if (!fs || !fs->dev || !buf) return -STRATUM_EINVAL;
    if (block_num >= fs->dev->capacity_sectors / STRATAFS_SECTORS_PER_BLOCK) return -STRATUM_EIO;
    uint64_t sector = block_num * STRATAFS_SECTORS_PER_BLOCK;
    int64_t wr = virtio_blk_write(fs->dev, sector, STRATAFS_SECTORS_PER_BLOCK, buf);
    return (wr == STRATAFS_BLOCK_SIZE) ? 0 : -STRATUM_EIO;
}

static inline int stratafs_read_block_tx(stratafs_t *fs, journal_tx_t *tx, uint64_t block_num, void *buf) {
    if (tx && journal_read_block(tx, block_num, buf) == 0) {
        return 0;
    }
    return stratafs_read_block(fs, block_num, buf);
}

static uint32_t superblock_checksum(stratafs_sb_t *sb) {
    uint32_t saved = sb->checksum;
    sb->checksum = 0;
    uint32_t crc = journal_crc32(sb, offsetof(stratafs_sb_t, checksum) + sizeof(sb->checksum));
    sb->checksum = saved;
    return crc;
}

int stratafs_format(virtio_blk_dev_t *dev) {
    if (!dev) return -STRATUM_EINVAL;

    kprintf("[STRATAFS] Formatting storage device with StrataFS layout...\n");

    uint64_t total_blocks = dev->capacity_sectors / STRATAFS_SECTORS_PER_BLOCK;
    if (total_blocks < 1024 || total_blocks > STRATAFS_MAX_BITMAP_BITS) {
        kprintf("[STRATAFS] Unsupported device size for StrataFS (%lu blocks)\n", total_blocks);
        return -STRATUM_ENOSPC;
    }

    /* 1. Initialize Superblock */
    stratafs_sb_t *sb = (stratafs_sb_t *)kmalloc(sizeof(stratafs_sb_t));
    if (!sb) return -STRATUM_ENOMEM;
    memset(sb, 0, sizeof(stratafs_sb_t));

    sb->magic = STRATAFS_MAGIC;
    sb->version = STRATAFS_VERSION;
    sb->block_size = STRATAFS_BLOCK_SIZE;
    sb->total_blocks = total_blocks;
    sb->total_inodes = 2048;
    sb->root_inode = STRATAFS_ROOT_INO;

    sb->block_bitmap_block = 2;
    sb->block_bitmap_blocks = 1;
    sb->inode_bitmap_block = 3;
    sb->inode_bitmap_blocks = 1;
    sb->inode_table_block = 4;
    sb->inode_table_blocks = 64; /* 64 blocks * 32 inodes/block = 2048 inodes */
    sb->journal_start_block = 68;
    sb->journal_block_count = 512;
    sb->data_start_block = 580;
    sb->data_block_count = (uint32_t)(total_blocks - sb->data_start_block);

    sb->free_blocks = sb->data_block_count;
    sb->free_inodes = sb->total_inodes;

    /* 2. Format Journal */
    int res = journal_format(dev, sb->journal_start_block, sb->journal_block_count);
    if (res != 0) {
        kfree(sb);
        return res;
    }

    /* 3. Initialize Block Bitmap (mark metadata blocks 0..580 as used) */
    uint8_t *bitmap = (uint8_t *)kmalloc(STRATAFS_BLOCK_SIZE);
    if (!bitmap) {
        kfree(sb);
        return -STRATUM_ENOMEM;
    }
    memset(bitmap, 0, STRATAFS_BLOCK_SIZE);

    /* Metadata blocks 0 to 579 are pre-allocated */
    for (uint32_t b = 0; b < sb->data_start_block; b++) {
        bitmap[b / 8] |= (uint8_t)(1 << (b % 8));
    }
    /* Root directory uses block 580 */
    bitmap[sb->data_start_block / 8] |= (uint8_t)(1 << (sb->data_start_block % 8));
    sb->free_blocks--;

    uint64_t bbm_sec = sb->block_bitmap_block * STRATAFS_SECTORS_PER_BLOCK;
    if (virtio_blk_write(dev, bbm_sec, STRATAFS_SECTORS_PER_BLOCK, bitmap) != STRATAFS_BLOCK_SIZE) goto io_error;

    /* 4. Initialize Inode Bitmap (mark inode 0 reserved, inode 1 root directory) */
    memset(bitmap, 0, STRATAFS_BLOCK_SIZE);
    bitmap[0] = 0x03; /* Inodes 0 and 1 marked allocated */
    sb->free_inodes -= 2;

    uint64_t ibm_sec = sb->inode_bitmap_block * STRATAFS_SECTORS_PER_BLOCK;
    if (virtio_blk_write(dev, ibm_sec, STRATAFS_SECTORS_PER_BLOCK, bitmap) != STRATAFS_BLOCK_SIZE) goto io_error;

    /* 5. Initialize Inode Table */
    uint8_t *zero_block = bitmap;
    memset(zero_block, 0, STRATAFS_BLOCK_SIZE);
    for (uint32_t i = 0; i < sb->inode_table_blocks; i++) {
        uint64_t it_sec = (sb->inode_table_block + i) * STRATAFS_SECTORS_PER_BLOCK;
        if (virtio_blk_write(dev, it_sec, STRATAFS_SECTORS_PER_BLOCK, zero_block) != STRATAFS_BLOCK_SIZE) goto io_error;
    }

    /* Write Root Inode (Inode 1) into Inode Table Block 4 */
    stratafs_disk_inode_t root_inode;
    memset(&root_inode, 0, sizeof(stratafs_disk_inode_t));
    root_inode.mode = VFS_MODE_IFDIR | 0755;
    root_inode.nlink = 2;
    root_inode.generation = 1;
    root_inode.size = STRATAFS_BLOCK_SIZE;
    root_inode.blocks = STRATAFS_SECTORS_PER_BLOCK;
    root_inode.direct[0] = sb->data_start_block;

    /* Root directory entry block (580) */
    memset(zero_block, 0, STRATAFS_BLOCK_SIZE);
    stratafs_disk_dirent_t *dot = (stratafs_disk_dirent_t *)zero_block;
    dot->inode_num = STRATAFS_ROOT_INO;
    dot->record_len = sizeof(stratafs_disk_dirent_t);
    dot->name_len = 1;
    dot->file_type = STRATAFS_FT_DIR;
    strcpy(dot->name, ".");

    stratafs_disk_dirent_t *dotdot = (stratafs_disk_dirent_t *)(zero_block + sizeof(stratafs_disk_dirent_t));
    dotdot->inode_num = STRATAFS_ROOT_INO;
    dotdot->record_len = sizeof(stratafs_disk_dirent_t);
    dotdot->name_len = 2;
    dotdot->file_type = STRATAFS_FT_DIR;
    strcpy(dotdot->name, "..");

    uint64_t root_data_sec = sb->data_start_block * STRATAFS_SECTORS_PER_BLOCK;
    if (virtio_blk_write(dev, root_data_sec, STRATAFS_SECTORS_PER_BLOCK, zero_block) != STRATAFS_BLOCK_SIZE) goto io_error;

    /* Write root inode to first inode table block */
    uint64_t it0_sec = sb->inode_table_block * STRATAFS_SECTORS_PER_BLOCK;
    memset(zero_block, 0, STRATAFS_BLOCK_SIZE);
    memcpy(zero_block + sizeof(stratafs_disk_inode_t), &root_inode, sizeof(stratafs_disk_inode_t));
    if (virtio_blk_write(dev, it0_sec, STRATAFS_SECTORS_PER_BLOCK, zero_block) != STRATAFS_BLOCK_SIZE) goto io_error;

    /* 6. Write Superblock */
    sb->checksum = superblock_checksum(sb);
    uint64_t sb_sec = 1 * STRATAFS_SECTORS_PER_BLOCK;
    if (virtio_blk_flush(dev) != 0 ||
        virtio_blk_write(dev, sb_sec, STRATAFS_SECTORS_PER_BLOCK, sb) != STRATAFS_BLOCK_SIZE ||
        virtio_blk_flush(dev) != 0) goto io_error;

    kfree(sb);
    kfree(bitmap);

    kprintf("[STRATAFS] Format complete: %lu total blocks, %lu free data blocks, 2048 inodes.\n",
            total_blocks, total_blocks - 581);
    return 0;
io_error:
    kfree(bitmap);
    kfree(sb);
    return -STRATUM_EIO;
}

int stratafs_alloc_block(stratafs_t *fs, journal_tx_t *tx, uint32_t *out_block) {
    if (!fs || !out_block) return -STRATUM_EINVAL;

    uint8_t *bitmap = (uint8_t *)kmalloc(STRATAFS_BLOCK_SIZE);
    if (!bitmap) return -STRATUM_ENOMEM;

    int res = stratafs_read_block_tx(fs, tx, fs->sb.block_bitmap_block, bitmap);
    if (res != 0) {
        kfree(bitmap);
        return res;
    }

    uint32_t start = fs->sb.data_start_block;
    uint32_t limit = (uint32_t)fs->sb.total_blocks;

    for (uint32_t b = start; b < limit; b++) {
        if (!(bitmap[b / 8] & (1 << (b % 8)))) {
            bitmap[b / 8] |= (uint8_t)(1 << (b % 8));

            res = journal_write_block(tx, fs->sb.block_bitmap_block, bitmap);

            *out_block = b;
            kfree(bitmap);
            return res;
        }
    }

    kfree(bitmap);
    return -STRATUM_ENOSPC;
}

int stratafs_free_block(stratafs_t *fs, journal_tx_t *tx, uint32_t block) {
    if (!fs || block < fs->sb.data_start_block || block >= fs->sb.total_blocks) {
        return -STRATUM_EINVAL;
    }

    uint8_t *bitmap = (uint8_t *)kmalloc(STRATAFS_BLOCK_SIZE);
    if (!bitmap) return -STRATUM_ENOMEM;

    int res = stratafs_read_block_tx(fs, tx, fs->sb.block_bitmap_block, bitmap);
    if (res != 0) {
        kfree(bitmap);
        return res;
    }

    if (!(bitmap[block / 8] & (1 << (block % 8)))) {
        kfree(bitmap);
        return -STRATUM_EIO;
    }
    bitmap[block / 8] &= (uint8_t)~(1 << (block % 8));

    res = journal_write_block(tx, fs->sb.block_bitmap_block, bitmap);
    kfree(bitmap);
    return res;
}

int stratafs_alloc_inode(stratafs_t *fs, journal_tx_t *tx, uint16_t mode, uint32_t *out_ino) {
    if (!fs || !out_ino) return -STRATUM_EINVAL;

    uint8_t *bitmap = (uint8_t *)kmalloc(STRATAFS_BLOCK_SIZE);
    if (!bitmap) return -STRATUM_ENOMEM;

    int res = stratafs_read_block_tx(fs, tx, fs->sb.inode_bitmap_block, bitmap);
    if (res != 0) {
        kfree(bitmap);
        return res;
    }

    for (uint32_t ino = 1; ino < fs->sb.total_inodes; ino++) {
        if (!(bitmap[ino / 8] & (1 << (ino % 8)))) {
            bitmap[ino / 8] |= (uint8_t)(1 << (ino % 8));

            res = journal_write_block(tx, fs->sb.inode_bitmap_block, bitmap);
            if (res != 0) break;

            stratafs_disk_inode_t dinode;
            res = stratafs_read_inode(fs, ino, &dinode);
            if (res != 0) break;
            uint32_t generation = dinode.generation + 1;
            memset(&dinode, 0, sizeof(dinode));
            dinode.generation = generation;
            dinode.mode = mode;
            dinode.nlink = (mode & VFS_MODE_IFDIR) ? 2 : 1;
            dinode.atime = timer_get_uptime_ms();
            dinode.mtime = dinode.atime;
            dinode.ctime = dinode.atime;

            res = stratafs_write_inode(fs, tx, ino, &dinode);

            *out_ino = ino;
            kfree(bitmap);
            return res;
        }
    }

    kfree(bitmap);
    return res ? res : -STRATUM_ENOSPC;
}

int stratafs_free_inode(stratafs_t *fs, journal_tx_t *tx, uint32_t ino) {
    if (!fs || ino < 1 || ino >= fs->sb.total_inodes) return -STRATUM_EINVAL;

    uint8_t *bitmap = (uint8_t *)kmalloc(STRATAFS_BLOCK_SIZE);
    if (!bitmap) return -STRATUM_ENOMEM;

    int res = stratafs_read_block_tx(fs, tx, fs->sb.inode_bitmap_block, bitmap);
    if (res != 0) {
        kfree(bitmap);
        return res;
    }

    bitmap[ino / 8] &= (uint8_t)~(1 << (ino % 8));

    res = journal_write_block(tx, fs->sb.inode_bitmap_block, bitmap);
    kfree(bitmap);
    if (res != 0) return res;

    stratafs_disk_inode_t old;
    res = stratafs_read_inode(fs, ino, &old);
    if (res != 0) return res;
    stratafs_disk_inode_t empty = { .generation = old.generation };
    return stratafs_write_inode(fs, tx, ino, &empty);
}

static int read_inode(stratafs_t *fs, journal_tx_t *tx, uint32_t ino, stratafs_disk_inode_t *out_inode) {
    if (!fs || ino >= fs->sb.total_inodes || !out_inode) return -STRATUM_EINVAL;

    uint32_t block_idx = fs->sb.inode_table_block + (ino / STRATAFS_INODES_PER_BLOCK);
    uint32_t offset = (ino % STRATAFS_INODES_PER_BLOCK) * STRATAFS_INODE_SIZE;

    uint8_t *buf = (uint8_t *)kmalloc(STRATAFS_BLOCK_SIZE);
    if (!buf) return -STRATUM_ENOMEM;

    int res = stratafs_read_block_tx(fs, tx, block_idx, buf);
    if (res == 0) {
        memcpy(out_inode, buf + offset, sizeof(stratafs_disk_inode_t));
    }
    kfree(buf);
    return res;
}

int stratafs_read_inode(stratafs_t *fs, uint32_t ino, stratafs_disk_inode_t *out_inode) {
    return read_inode(fs, NULL, ino, out_inode);
}

int stratafs_write_inode(stratafs_t *fs, journal_tx_t *tx, uint32_t ino, const stratafs_disk_inode_t *inode) {
    if (!fs || ino >= fs->sb.total_inodes || !inode) return -STRATUM_EINVAL;

    uint32_t block_idx = fs->sb.inode_table_block + (ino / STRATAFS_INODES_PER_BLOCK);
    uint32_t offset = (ino % STRATAFS_INODES_PER_BLOCK) * STRATAFS_INODE_SIZE;

    uint8_t *buf = (uint8_t *)kmalloc(STRATAFS_BLOCK_SIZE);
    if (!buf) return -STRATUM_ENOMEM;

    int res = stratafs_read_block_tx(fs, tx, block_idx, buf);
    if (res == 0) {
        memcpy(buf + offset, inode, sizeof(stratafs_disk_inode_t));
        if (tx) {
            res = journal_write_block(tx, block_idx, buf);
        } else {
            res = stratafs_write_block(fs, block_idx, buf);
        }
    }
    kfree(buf);
    return res;
}

static bool valid_data_block(const stratafs_t *fs, uint32_t block) {
    return block >= fs->sb.data_start_block && block < fs->sb.total_blocks;
}

static int alloc_zero_block(stratafs_t *fs, journal_tx_t *tx, uint32_t *block) {
    static const uint8_t zero[STRATAFS_BLOCK_SIZE];
    int res = stratafs_alloc_block(fs, tx, block);
    return res ? res : journal_write_block(tx, *block, zero);
}

static int stratafs_get_block_mapping(stratafs_t *fs, journal_tx_t *tx, stratafs_disk_inode_t *inode,
                                      uint32_t index, bool allocate, uint32_t *out_phys_block) {
    if (index < STRATAFS_NDIR_BLOCKS) {
        uint32_t block = inode->direct[index];
        if (!block && allocate) {
            int res = alloc_zero_block(fs, tx, &block);
            if (res != 0) return res;
            inode->direct[index] = block;
            inode->blocks += STRATAFS_SECTORS_PER_BLOCK;
        }
        if (block && !valid_data_block(fs, block)) return -STRATUM_EIO;
        *out_phys_block = block;
        return 0;
    }

    index -= STRATAFS_NDIR_BLOCKS;
    if (index >= STRATAFS_INDIR_BLOCKS) return -STRATUM_EFBIG;
    if (!inode->indirect) {
        if (!allocate) {
            *out_phys_block = 0;
            return 0;
        }
        uint32_t indirect;
        int res = alloc_zero_block(fs, tx, &indirect);
        if (res != 0) return res;
        inode->indirect = indirect;
        inode->blocks += STRATAFS_SECTORS_PER_BLOCK;
    }
    if (!valid_data_block(fs, inode->indirect)) return -STRATUM_EIO;
    uint32_t *entries = kmalloc(STRATAFS_BLOCK_SIZE);
    if (!entries) return -STRATUM_ENOMEM;
    int res = stratafs_read_block_tx(fs, tx, inode->indirect, entries);
    if (res == 0 && !entries[index] && allocate) {
        res = alloc_zero_block(fs, tx, &entries[index]);
        if (res == 0) res = journal_write_block(tx, inode->indirect, entries);
        if (res == 0) inode->blocks += STRATAFS_SECTORS_PER_BLOCK;
    }
    if (res == 0) {
        *out_phys_block = entries[index];
        if (entries[index] && !valid_data_block(fs, entries[index])) res = -STRATUM_EIO;
    }
    kfree(entries);
    return res;
}

int64_t stratafs_read_file(stratafs_t *fs, stratafs_disk_inode_t *inode, uint64_t offset, void *buf, size_t count) {
    if (!fs || !inode || !buf) return -STRATUM_EINVAL;
    if (offset >= inode->size) return 0;

    size_t to_read = count;
    if (to_read > inode->size - offset) {
        to_read = (size_t)(inode->size - offset);
    }

    uint8_t *dst = (uint8_t *)buf;
    size_t bytes_read = 0;
    uint8_t *block_buf = (uint8_t *)kmalloc(STRATAFS_BLOCK_SIZE);
    if (!block_buf) return -STRATUM_ENOMEM;

    while (bytes_read < to_read) {
        uint64_t cur_pos = offset + bytes_read;
        uint32_t blk_idx = (uint32_t)(cur_pos / STRATAFS_BLOCK_SIZE);
        uint32_t blk_off = (uint32_t)(cur_pos % STRATAFS_BLOCK_SIZE);
        size_t chunk = STRATAFS_BLOCK_SIZE - blk_off;
        if (chunk > to_read - bytes_read) {
            chunk = to_read - bytes_read;
        }

        uint32_t phys_blk = 0;
        int m_res = stratafs_get_block_mapping(fs, NULL, inode, blk_idx, false, &phys_blk);
        if (m_res != 0) {
            kfree(block_buf);
            return (bytes_read > 0) ? (int64_t)bytes_read : m_res;
        }

        if (phys_blk != 0) {
            int res = stratafs_read_block(fs, phys_blk, block_buf);
            if (res != 0) {
                kfree(block_buf);
                return bytes_read ? (int64_t)bytes_read : res;
            }
            memcpy(dst + bytes_read, block_buf + blk_off, chunk);
        } else {
            memset(dst + bytes_read, 0, chunk); /* Sparse hole */
        }

        bytes_read += chunk;
    }

    kfree(block_buf);
    return (int64_t)bytes_read;
}

int64_t stratafs_write_file(stratafs_t *fs, journal_tx_t *tx, uint32_t ino, stratafs_disk_inode_t *inode,
                            uint64_t offset, const void *buf, size_t count) {
    if (!fs || !tx || !inode || !buf) return -STRATUM_EINVAL;

    if (offset > STRATAFS_MAX_FILE_SIZE || count > STRATAFS_MAX_FILE_SIZE - offset) {
        return -STRATUM_EFBIG;
    }
    if (count == 0) return 0;

    const uint8_t *src = (const uint8_t *)buf;
    size_t bytes_written = 0;
    uint8_t *block_buf = (uint8_t *)kmalloc(STRATAFS_BLOCK_SIZE);
    if (!block_buf) return -STRATUM_ENOMEM;

    while (bytes_written < count) {
        uint64_t cur_pos = offset + bytes_written;
        uint32_t blk_idx = (uint32_t)(cur_pos / STRATAFS_BLOCK_SIZE);
        uint32_t blk_off = (uint32_t)(cur_pos % STRATAFS_BLOCK_SIZE);
        size_t chunk = STRATAFS_BLOCK_SIZE - blk_off;
        if (chunk > count - bytes_written) {
            chunk = count - bytes_written;
        }

        uint32_t phys_blk = 0;
        int m_res = stratafs_get_block_mapping(fs, tx, inode, blk_idx, true, &phys_blk);
        if (m_res != 0) {
            kfree(block_buf);
            return m_res;
        }

        if (chunk == STRATAFS_BLOCK_SIZE) {
            memcpy(block_buf, src + bytes_written, chunk);
        } else {
            int res = stratafs_read_block_tx(fs, tx, phys_blk, block_buf);
            if (res != 0) {
                kfree(block_buf);
                return res;
            }
            memcpy(block_buf + blk_off, src + bytes_written, chunk);
        }

        int res = journal_write_block(tx, phys_blk, block_buf);
        if (res != 0) {
            kfree(block_buf);
            return res;
        }
        bytes_written += chunk;
    }

    if (offset + bytes_written > inode->size) {
        inode->size = offset + bytes_written;
    }
    inode->mtime = timer_get_uptime_ms();
    int res = stratafs_write_inode(fs, tx, ino, inode);
    kfree(block_buf);
    return res ? res : (int64_t)bytes_written;
}

static bool is_directory(const stratafs_disk_inode_t *inode) {
    return (inode->mode & STRATAFS_MODE_MASK) == VFS_MODE_IFDIR;
}

static int find_dirent(stratafs_t *fs, journal_tx_t *tx, stratafs_disk_inode_t *dir,
                       const char *name, uint8_t *buf, uint32_t *block, uint32_t *index,
                       bool find_empty) {
    if (!is_directory(dir)) return -STRATUM_ENOTDIR;
    if (dir->size == 0 || dir->size > STRATAFS_MAX_FILE_SIZE || dir->size % STRATAFS_BLOCK_SIZE) {
        return -STRATUM_EIO;
    }
    uint32_t blocks = (uint32_t)(dir->size / STRATAFS_BLOCK_SIZE);
    size_t name_len = strlen(name);
    for (uint32_t b = 0; b < blocks; b++) {
        int res = stratafs_get_block_mapping(fs, tx, dir, b, false, block);
        if (res != 0) return res;
        if (*block == 0) return -STRATUM_EIO;
        res = stratafs_read_block_tx(fs, tx, *block, buf);
        if (res != 0) return res;
        stratafs_disk_dirent_t *entries = (stratafs_disk_dirent_t *)buf;
        for (uint32_t i = 0; i < STRATAFS_BLOCK_SIZE / sizeof(*entries); i++) {
            const stratafs_disk_dirent_t *de = &entries[i];
            if (!stratafs_valid_dirent(de, fs->sb.total_inodes)) return -STRATUM_EIO;
            if ((find_empty && de->inode_num == 0) ||
                (!find_empty && de->inode_num != 0 && de->name_len == name_len &&
                 memcmp(de->name, name, name_len) == 0)) {
                *index = i;
                return 0;
            }
        }
    }
    return -STRATUM_ENOENT;
}

int stratafs_dir_lookup(stratafs_t *fs, stratafs_disk_inode_t *dir, const char *name, uint32_t *out_ino) {
    uint8_t *buf = kmalloc(STRATAFS_BLOCK_SIZE);
    if (!buf) return -STRATUM_ENOMEM;
    uint32_t block, index;
    int res = find_dirent(fs, NULL, dir, name, buf, &block, &index, false);
    if (res == 0) *out_ino = ((stratafs_disk_dirent_t *)buf)[index].inode_num;
    kfree(buf);
    return res;
}

static void init_dirent(stratafs_disk_dirent_t *entry, const char *name, uint32_t ino, uint8_t type) {
    memset(entry, 0, sizeof(*entry));
    entry->inode_num = ino;
    entry->record_len = sizeof(*entry);
    entry->name_len = (uint8_t)strlen(name);
    entry->file_type = type;
    memcpy(entry->name, name, entry->name_len);
}

int stratafs_dir_add(stratafs_t *fs, journal_tx_t *tx, uint32_t dir_ino, stratafs_disk_inode_t *dir,
                     const char *name, uint32_t ino, uint8_t file_type) {
    if (strlen(name) >= sizeof(((stratafs_disk_dirent_t *)0)->name)) return -STRATUM_ENAMETOOLONG;
    uint8_t *buf = kmalloc(STRATAFS_BLOCK_SIZE);
    if (!buf) return -STRATUM_ENOMEM;
    uint32_t block, index;
    int res = find_dirent(fs, tx, dir, name, buf, &block, &index, true);
    if (res == -STRATUM_ENOENT) {
        res = stratafs_get_block_mapping(fs, tx, dir, (uint32_t)(dir->size / STRATAFS_BLOCK_SIZE), true, &block);
        if (res == 0) {
            index = 0;
            memset(buf, 0, STRATAFS_BLOCK_SIZE);
            dir->size += STRATAFS_BLOCK_SIZE;
            res = stratafs_write_inode(fs, tx, dir_ino, dir);
        }
    }
    if (res == 0) {
        init_dirent(&((stratafs_disk_dirent_t *)buf)[index], name, ino, file_type);
        res = journal_write_block(tx, block, buf);
    }
    kfree(buf);
    return res;
}

static int remove_dirent(stratafs_t *fs, journal_tx_t *tx, stratafs_disk_inode_t *dir, const char *name) {
    uint8_t *buf = kmalloc(STRATAFS_BLOCK_SIZE);
    if (!buf) return -STRATUM_ENOMEM;
    uint32_t block, index;
    int res = find_dirent(fs, tx, dir, name, buf, &block, &index, false);
    if (res == 0) {
        memset(&((stratafs_disk_dirent_t *)buf)[index], 0, sizeof(stratafs_disk_dirent_t));
        res = journal_write_block(tx, block, buf);
    }
    kfree(buf);
    return res;
}

static uint32_t count_free(const uint8_t *bitmap, uint32_t limit) {
    uint32_t free_count = 0;
    for (uint32_t bit = 0; bit < limit; bit++) {
        if (!(bitmap[bit / 8] & (1U << (bit % 8)))) free_count++;
    }
    return free_count;
}

static int finish_tx(stratafs_t *fs, journal_tx_t *tx, int res) {
    if (!tx) return res;
    if (res != 0) {
        journal_abort(tx);
        return res;
    }

    uint8_t *scratch = kmalloc(STRATAFS_BLOCK_SIZE);
    if (!scratch) {
        journal_abort(tx);
        return -STRATUM_ENOMEM;
    }
    uint64_t free_blocks = fs->sb.free_blocks;
    uint32_t free_inodes = fs->sb.free_inodes;
    bool updated = false;
    if (journal_read_block(tx, fs->sb.block_bitmap_block, scratch) == 0) {
        free_blocks = count_free(scratch, (uint32_t)fs->sb.total_blocks);
        updated = true;
    }
    if (journal_read_block(tx, fs->sb.inode_bitmap_block, scratch) == 0) {
        free_inodes = count_free(scratch, fs->sb.total_inodes);
        updated = true;
    }
    if (updated) {
        stratafs_sb_t *sb = (stratafs_sb_t *)scratch;
        *sb = fs->sb;
        sb->free_blocks = free_blocks;
        sb->free_inodes = free_inodes;
        sb->checksum = superblock_checksum(sb);
        res = journal_write_block(tx, 1, sb);
    }
    kfree(scratch);
    if (res != 0) {
        journal_abort(tx);
        return res;
    }

    res = journal_commit(&fs->journal, tx);
    if (res == 0) {
        fs->sb.free_blocks = free_blocks;
        fs->sb.free_inodes = free_inodes;
    }
    return res;
}

static int load_node(stratafs_t *fs, const vfs_node_t *node, stratafs_disk_inode_t *inode) {
    if (!fs->journal.active) return -STRATUM_EIO;
    int res = stratafs_read_inode(fs, (uint32_t)node->inode_num, inode);
    if (res != 0) return res;
    return inode->mode && inode->generation == node->generation ? 0 : -STRATUM_ENOENT;
}

static int stratafs_vfs_lookup(vfs_node_t *parent, const char *name, vfs_node_t **out_child) {
    stratafs_t *fs = parent->mount->fs_priv;
    uint64_t flags;
    spin_lock_irqsave(&fs->lock, &flags);
    stratafs_disk_inode_t dir, inode;
    uint32_t ino;
    int res = load_node(fs, parent, &dir);
    if (res == 0) res = stratafs_dir_lookup(fs, &dir, name, &ino);
    if (res == 0) res = stratafs_read_inode(fs, ino, &inode);
    if (res == 0 && !inode.mode) res = -STRATUM_EIO;
    if (res == 0) {
        *out_child = stratafs_create_vfs_node(fs, ino, &inode, name);
        if (!*out_child) res = -STRATUM_ENOMEM;
    }
    spin_unlock_irqrestore(&fs->lock, flags);
    return res;
}

static int create_entry(vfs_node_t *parent, const char *name, uint16_t mode, vfs_node_t **out_child) {
    stratafs_t *fs = parent->mount->fs_priv;
    uint64_t flags;
    spin_lock_irqsave(&fs->lock, &flags);
    stratafs_disk_inode_t dir, inode;
    journal_tx_t *tx = NULL;
    vfs_node_t *child = NULL;
    uint32_t ino;
    int res = load_node(fs, parent, &dir);
    if (res != 0) goto out;
    res = stratafs_dir_lookup(fs, &dir, name, &ino);
    if (res == 0) res = -STRATUM_EEXIST;
    if (res != -STRATUM_ENOENT) goto out;
    res = journal_begin(&fs->journal, &tx);
    if (res != 0) goto out;
    res = stratafs_alloc_inode(fs, tx, mode, &ino);
    if (res != 0) goto out;
    res = read_inode(fs, tx, ino, &inode);
    if (res != 0) goto out;
    if (is_directory(&inode)) {
        uint32_t block;
        res = stratafs_alloc_block(fs, tx, &block);
        if (res != 0) goto out;
        stratafs_disk_dirent_t *entries = kmalloc(STRATAFS_BLOCK_SIZE);
        if (!entries) {
            res = -STRATUM_ENOMEM;
            goto out;
        }
        memset(entries, 0, STRATAFS_BLOCK_SIZE);
        init_dirent(&entries[0], ".", ino, STRATAFS_FT_DIR);
        init_dirent(&entries[1], "..", (uint32_t)parent->inode_num, STRATAFS_FT_DIR);
        res = journal_write_block(tx, block, entries);
        kfree(entries);
        if (res != 0) goto out;
        inode.direct[0] = block;
        inode.size = STRATAFS_BLOCK_SIZE;
        inode.blocks = STRATAFS_SECTORS_PER_BLOCK;
        res = stratafs_write_inode(fs, tx, ino, &inode);
        if (res != 0) goto out;
        dir.nlink++;
        res = stratafs_write_inode(fs, tx, (uint32_t)parent->inode_num, &dir);
        if (res != 0) goto out;
    }

    res = stratafs_dir_add(fs, tx, (uint32_t)parent->inode_num, &dir, name, ino,
                          is_directory(&inode) ? STRATAFS_FT_DIR : STRATAFS_FT_REG);
    if (res == 0) {
        child = stratafs_create_vfs_node(fs, ino, &inode, name);
        if (!child) res = -STRATUM_ENOMEM;
    }
out:
    res = finish_tx(fs, tx, res);
    spin_unlock_irqrestore(&fs->lock, flags);
    if (res == 0) *out_child = child;
    else vfs_node_release(child);
    return res;
}

static int stratafs_vfs_create(vfs_node_t *parent, const char *name, uint32_t mode, vfs_node_t **out_child) {
    return create_entry(parent, name, (uint16_t)(VFS_MODE_IFREG | (mode & 0777)), out_child);
}

static int stratafs_vfs_mkdir(vfs_node_t *parent, const char *name, uint32_t mode, vfs_node_t **out_child) {
    return create_entry(parent, name, (uint16_t)(VFS_MODE_IFDIR | (mode & 0777)), out_child);
}

static int free_file_blocks(stratafs_t *fs, journal_tx_t *tx, stratafs_disk_inode_t *inode) {
    if (inode->double_indirect) return -STRATUM_EIO;
    for (uint32_t i = 0; i < STRATAFS_NDIR_BLOCKS; i++) {
        if (!inode->direct[i]) continue;
        int res = stratafs_free_block(fs, tx, inode->direct[i]);
        if (res != 0) return res;
        inode->direct[i] = 0;
    }
    if (inode->indirect) {
        if (!valid_data_block(fs, inode->indirect)) return -STRATUM_EIO;
        uint32_t *entries = kmalloc(STRATAFS_BLOCK_SIZE);
        if (!entries) return -STRATUM_ENOMEM;
        int res = stratafs_read_block(fs, inode->indirect, entries);
        for (uint32_t i = 0; res == 0 && i < STRATAFS_INDIR_BLOCKS; i++) {
            if (entries[i]) res = stratafs_free_block(fs, tx, entries[i]);
        }
        kfree(entries);
        if (res != 0) return res;
        res = stratafs_free_block(fs, tx, inode->indirect);
        if (res != 0) return res;
        inode->indirect = 0;
    }
    inode->blocks = 0;
    inode->size = 0;
    return 0;
}

static int stratafs_vfs_unlink(vfs_node_t *parent, const char *name) {
    stratafs_t *fs = parent->mount->fs_priv;
    uint64_t flags;
    spin_lock_irqsave(&fs->lock, &flags);
    stratafs_disk_inode_t dir, inode;
    uint32_t ino = 0;
    journal_tx_t *tx = NULL;
    int res = load_node(fs, parent, &dir);
    if (res == 0) res = stratafs_dir_lookup(fs, &dir, name, &ino);
    if (res == 0) res = stratafs_read_inode(fs, ino, &inode);
    if (res == 0 && (is_directory(&inode) || !inode.mode)) res = -STRATUM_EISDIR;
    if (res == 0 && fs->open_count[ino]) res = -STRATUM_EBUSY;
    if (res == 0) res = journal_begin(&fs->journal, &tx);
    if (res == 0) res = remove_dirent(fs, tx, &dir, name);
    if (res == 0) res = free_file_blocks(fs, tx, &inode);
    if (res == 0) res = stratafs_free_inode(fs, tx, ino);
    res = finish_tx(fs, tx, res);
    spin_unlock_irqrestore(&fs->lock, flags);
    return res;
}

static int stratafs_vfs_rename(vfs_node_t *old_parent, const char *old_name,
                               vfs_node_t *new_parent, const char *new_name) {
    stratafs_t *fs = old_parent->mount->fs_priv;
    uint64_t flags;
    spin_lock_irqsave(&fs->lock, &flags);

    stratafs_disk_inode_t old_dir, new_dir, source, replaced;
    bool same_parent = old_parent->inode_num == new_parent->inode_num;
    stratafs_disk_inode_t *destination = same_parent ? &old_dir : &new_dir;
    journal_tx_t *tx = NULL;
    uint32_t src, dst;

    int res = load_node(fs, old_parent, &old_dir);
    if (res != 0) goto out;
    if (!same_parent) {
        res = load_node(fs, new_parent, &new_dir);
        if (res != 0) goto out;
    }
    res = stratafs_dir_lookup(fs, &old_dir, old_name, &src);
    if (res != 0) goto out;
    if (same_parent && strcmp(old_name, new_name) == 0) goto out;
    res = stratafs_read_inode(fs, src, &source);
    if (res != 0) goto out;
    if ((source.mode & STRATAFS_MODE_MASK) != VFS_MODE_IFREG) {
        res = -STRATUM_EISDIR;
        goto out;
    }

    res = stratafs_dir_lookup(fs, destination, new_name, &dst);
    bool replace = res == 0;
    if (!replace && res != -STRATUM_ENOENT) goto out;
    if (replace) {
        if (src == dst) goto out;
        res = stratafs_read_inode(fs, dst, &replaced);
        if (res != 0) goto out;
        if ((replaced.mode & STRATAFS_MODE_MASK) != VFS_MODE_IFREG) {
            res = -STRATUM_EISDIR;
            goto out;
        }
        if (fs->open_count[dst]) {
            res = -STRATUM_EBUSY;
            goto out;
        }
    }

    res = journal_begin(&fs->journal, &tx);
    if (res == 0 && replace) {
        res = remove_dirent(fs, tx, destination, new_name);
        if (res == 0) res = free_file_blocks(fs, tx, &replaced);
        if (res == 0) res = stratafs_free_inode(fs, tx, dst);
    }
    if (res == 0) res = remove_dirent(fs, tx, &old_dir, old_name);
    if (res == 0) {
        res = stratafs_dir_add(fs, tx, (uint32_t)new_parent->inode_num,
                              destination, new_name, src, STRATAFS_FT_REG);
    }
out:
    res = finish_tx(fs, tx, res);
    spin_unlock_irqrestore(&fs->lock, flags);
    return res;
}

static int stratafs_vfs_readdir(vfs_node_t *node, uint64_t *index, vfs_dirent_t *entry) {
    stratafs_t *fs = node->mount->fs_priv;
    uint64_t flags;
    spin_lock_irqsave(&fs->lock, &flags);
    stratafs_disk_inode_t inode;
    int res = load_node(fs, node, &inode);
    if (res == 0 && !is_directory(&inode)) res = -STRATUM_ENOTDIR;
    if (res == 0 && (inode.size > STRATAFS_MAX_FILE_SIZE || inode.size % STRATAFS_BLOCK_SIZE)) res = -STRATUM_EIO;

    stratafs_disk_dirent_t *entries = NULL;
    if (res == 0 && *index < inode.size / sizeof(*entries)) {
        entries = kmalloc(STRATAFS_BLOCK_SIZE);
        if (!entries) res = -STRATUM_ENOMEM;
    }
    uint64_t slots = STRATAFS_BLOCK_SIZE / sizeof(*entries);
    while (res == 0 && entries && *index < inode.size / sizeof(*entries)) {
        uint32_t block;
        res = stratafs_get_block_mapping(fs, NULL, &inode, (uint32_t)(*index / slots), false, &block);
        if (res == 0) res = block ? stratafs_read_block(fs, block, entries) : -STRATUM_EIO;
        if (res != 0) break;
        do {
            const stratafs_disk_dirent_t *record = &entries[*index % slots];
            (*index)++;
            if (!stratafs_valid_dirent(record, fs->sb.total_inodes)) {
                res = -STRATUM_EIO;
                break;
            }
            if (record->inode_num) {
                entry->d_ino = record->inode_num;
                entry->d_type = record->file_type;
                memcpy(entry->d_name, record->name, record->name_len);
                entry->d_name[record->name_len] = 0;
                res = 1;
                break;
            }
        } while (*index % slots != 0);
    }
    kfree(entries);
    spin_unlock_irqrestore(&fs->lock, flags);
    return res;
}

static int stratafs_vfs_open(vfs_node_t *node, uint32_t open_flags) {
    stratafs_t *fs = node->mount->fs_priv;
    uint64_t flags;
    spin_lock_irqsave(&fs->lock, &flags);
    stratafs_disk_inode_t inode;
    int res = load_node(fs, node, &inode);
    if (res == 0 && is_directory(&inode) && !(open_flags & O_DIRECTORY)) res = -STRATUM_EISDIR;
    if (res == 0 && (open_flags & O_TRUNC)) {
        journal_tx_t *tx = NULL;
        res = journal_begin(&fs->journal, &tx);
        if (res == 0) res = free_file_blocks(fs, tx, &inode);
        if (res == 0) res = stratafs_write_inode(fs, tx, (uint32_t)node->inode_num, &inode);
        res = finish_tx(fs, tx, res);
    }
    if (res == 0) {
        fs->open_count[node->inode_num]++;
        node->size = inode.size;
    }
    spin_unlock_irqrestore(&fs->lock, flags);
    return res;
}

static int stratafs_vfs_close(vfs_node_t *node) {
    stratafs_t *fs = node->mount->fs_priv;
    uint64_t flags;
    spin_lock_irqsave(&fs->lock, &flags);
    fs->open_count[node->inode_num]--;
    spin_unlock_irqrestore(&fs->lock, flags);
    return 0;
}

static int64_t stratafs_vfs_read(vfs_node_t *node, uint64_t offset, void *buf, size_t count) {
    stratafs_t *fs = node->mount->fs_priv;
    uint64_t flags;
    spin_lock_irqsave(&fs->lock, &flags);
    stratafs_disk_inode_t inode;
    int res = load_node(fs, node, &inode);
    int64_t read = res ? res : stratafs_read_file(fs, &inode, offset, buf, count);
    spin_unlock_irqrestore(&fs->lock, flags);
    return read;
}

static int64_t stratafs_vfs_write(vfs_node_t *node, uint64_t offset, const void *buf, size_t count) {
    stratafs_t *fs = node->mount->fs_priv;
    uint64_t flags;
    spin_lock_irqsave(&fs->lock, &flags);
    stratafs_disk_inode_t inode;
    journal_tx_t *tx = NULL;
    int res = load_node(fs, node, &inode);
    if (res == 0) res = journal_begin(&fs->journal, &tx);
    if (res == 0 && offset == VFS_APPEND_OFFSET) offset = inode.size;
    int64_t written = res ? res : stratafs_write_file(fs, tx, (uint32_t)node->inode_num, &inode,
                                                      offset, buf, count);
    res = finish_tx(fs, tx, written < 0 ? (int)written : 0);
    if (res == 0 && written > 0) node->size = inode.size;
    spin_unlock_irqrestore(&fs->lock, flags);
    return res ? res : written;
}

static int stratafs_vfs_stat(vfs_node_t *node, vfs_stat_t *out_st) {
    stratafs_t *fs = (stratafs_t *)node->mount->fs_priv;
    stratafs_disk_inode_t dinode;
    uint64_t flags;
    spin_lock_irqsave(&fs->lock, &flags);
    int res = load_node(fs, node, &dinode);
    spin_unlock_irqrestore(&fs->lock, flags);
    if (res != 0) return res;

    memset(out_st, 0, sizeof(vfs_stat_t));
    out_st->st_ino = node->inode_num;
    out_st->st_mode = dinode.mode;
    out_st->st_nlink = dinode.nlink;
    out_st->st_uid = dinode.uid;
    out_st->st_gid = dinode.gid;
    out_st->st_size = dinode.size;
    out_st->st_blksize = STRATAFS_BLOCK_SIZE;
    out_st->st_blocks = dinode.blocks;
    out_st->st_atime = dinode.atime;
    out_st->st_mtime = dinode.mtime;
    out_st->st_ctime = dinode.ctime;
    return 0;
}

static int stratafs_vfs_sync(vfs_node_t *node) {
    stratafs_t *fs = (stratafs_t *)node->mount->fs_priv;
    uint64_t flags;
    spin_lock_irqsave(&fs->lock, &flags);
    int res = journal_checkpoint(&fs->journal);
    spin_unlock_irqrestore(&fs->lock, flags);
    return res;
}

const vfs_node_ops_t g_stratafs_node_ops = {
    .open     = stratafs_vfs_open,
    .close    = stratafs_vfs_close,
    .read     = stratafs_vfs_read,
    .write    = stratafs_vfs_write,
    .lookup   = stratafs_vfs_lookup,
    .create   = stratafs_vfs_create,
    .mkdir    = stratafs_vfs_mkdir,
    .unlink   = stratafs_vfs_unlink,
    .rename   = stratafs_vfs_rename,
    .readdir  = stratafs_vfs_readdir,
    .stat     = stratafs_vfs_stat,
    .sync     = stratafs_vfs_sync,
    .truncate = NULL,
};

static vfs_node_t *stratafs_create_vfs_node(stratafs_t *fs, uint32_t ino, const stratafs_disk_inode_t *dinode, const char *name) {
    vfs_node_t *node = vfs_node_alloc();
    if (!node) return NULL;

    strncpy(node->name, name, sizeof(node->name) - 1);
    node->inode_num = ino;
    node->generation = dinode->generation;
    node->mount = fs->mount;
    node->type = (dinode->mode & VFS_MODE_IFDIR) ? VFS_TYPE_DIR : VFS_TYPE_REG;
    node->mode = dinode->mode;
    node->size = dinode->size;
    node->atime = dinode->atime;
    node->mtime = dinode->mtime;
    node->ctime = dinode->ctime;
    node->ops = &g_stratafs_node_ops;

    return node;
}

static bool valid_superblock(stratafs_t *fs) {
    stratafs_sb_t *sb = &fs->sb;
    return sb->magic == STRATAFS_MAGIC && sb->version == STRATAFS_VERSION &&
        sb->block_size == STRATAFS_BLOCK_SIZE && sb->total_blocks >= 1024 &&
        sb->total_blocks <= STRATAFS_MAX_BITMAP_BITS &&
        sb->total_blocks <= fs->dev->capacity_sectors / STRATAFS_SECTORS_PER_BLOCK &&
        sb->root_inode == STRATAFS_ROOT_INO && sb->total_inodes == 2048 &&
        sb->block_bitmap_block == 2 && sb->block_bitmap_blocks == 1 &&
        sb->inode_bitmap_block == 3 && sb->inode_bitmap_blocks == 1 &&
        sb->inode_table_block == 4 && sb->inode_table_blocks == 64 &&
        sb->journal_start_block == 68 && sb->journal_block_count == 512 &&
        sb->data_start_block == 580 && sb->data_block_count == sb->total_blocks - 580 &&
        sb->checksum == superblock_checksum(sb);
}

static bool blank_volume(stratafs_t *fs) {
    uint8_t *buf = kmalloc(STRATAFS_BLOCK_SIZE);
    if (!buf) return false;
    bool blank = true;
    for (uint64_t block = 0; blank && block < fs->dev->capacity_sectors / STRATAFS_SECTORS_PER_BLOCK; block++) {
        if (stratafs_read_block(fs, block, buf) != 0) {
            blank = false;
            break;
        }
        for (size_t i = 0; i < STRATAFS_BLOCK_SIZE; i++) {
            if (buf[i]) {
                blank = false;
                break;
            }
        }
    }
    kfree(buf);
    return blank;
}

int stratafs_mount_fs(vfs_mount_t *mnt, const char *dev_name) {
    (void)dev_name;
    virtio_blk_dev_t *blk = virtio_blk_get_primary();
    if (!blk) {
        kprintf("[STRATAFS] Primary Virtio-blk device not found!\n");
        return -STRATUM_ENODEV;
    }

    if (g_stratafs_active) return -STRATUM_EBUSY;
    uint64_t blocks = blk->capacity_sectors / STRATAFS_SECTORS_PER_BLOCK;
    if (blocks < 1024 || blocks > STRATAFS_MAX_BITMAP_BITS) return -STRATUM_ENOSPC;
    stratafs_t *fs = &g_stratafs;
    memset(fs, 0, sizeof(stratafs_t));
    fs->dev = blk;
    spin_lock_init(&fs->lock);
    fs->mount = mnt;

    int res = stratafs_read_block(fs, 1, &fs->sb);
    if (res != 0) return res;
    if (fs->sb.magic == 0 && blank_volume(fs)) {
        res = stratafs_format(blk);
        if (res != 0) return res;
        res = stratafs_read_block(fs, 1, &fs->sb);
        if (res != 0) return res;
    }
    if (!valid_superblock(fs)) {
        kprintf("[STRATAFS] Invalid superblock. Refusing to format a nonblank volume.\n");
        return -STRATUM_EIO;
    }

    /* Initialize Journal and crash recovery */
    res = journal_init(blk, fs->sb.journal_start_block, fs->sb.journal_block_count, &fs->journal);
    if (res != 0) {
        kprintf("[STRATAFS] Journal initialization failed: %d\n", res);
        return res;
    }

    /* Recovery can replay the filesystem superblock along with its bitmaps. */
    res = stratafs_read_block(fs, 1, &fs->sb);
    if (res != 0 || !valid_superblock(fs)) return -STRATUM_EIO;

    /* Create Root Node */
    stratafs_disk_inode_t root_dinode;
    res = stratafs_read_inode(fs, STRATAFS_ROOT_INO, &root_dinode);
    if (res != 0) return res;

    if (!is_directory(&root_dinode) || !valid_data_block(fs, root_dinode.direct[0])) {
        kprintf("[STRATAFS] Invalid root inode. Volume requires offline inspection.\n");
        return -STRATUM_EIO;
    }

    vfs_node_t *root_node = stratafs_create_vfs_node(fs, STRATAFS_ROOT_INO, &root_dinode, "/");
    if (!root_node) return -STRATUM_ENOMEM;

    root_node->mount = mnt;
    mnt->root_node = root_node;
    mnt->fs_priv = fs;
    g_stratafs_active = true;

    kprintf("[STRATAFS] Mounted StrataFS on '%s' (%lu MiB volume, %u free blocks)\n",
            mnt->mountpoint, (fs->sb.total_blocks * STRATAFS_BLOCK_SIZE) / (1024 * 1024),
            (uint32_t)fs->sb.free_blocks);
    return 0;
}

static int stratafs_mount_op(vfs_mount_t *mnt, const char *dev) {
    return stratafs_mount_fs(mnt, dev);
}

static int stratafs_unmount_op(vfs_mount_t *mnt) {
    if (!mnt || !mnt->fs_priv) return -STRATUM_EINVAL;
    stratafs_t *fs = (stratafs_t *)mnt->fs_priv;
    int res = journal_checkpoint(&fs->journal);
    if (res == 0) g_stratafs_active = false;
    return res;
}

static int stratafs_sync_op(vfs_mount_t *mnt) {
    if (!mnt || !mnt->fs_priv) return -STRATUM_EINVAL;
    stratafs_t *fs = (stratafs_t *)mnt->fs_priv;
    uint64_t flags;
    spin_lock_irqsave(&fs->lock, &flags);
    int res = journal_checkpoint(&fs->journal);
    spin_unlock_irqrestore(&fs->lock, flags);
    return res;
}

static const vfs_mount_ops_t g_stratafs_mount_ops = {
    .mount   = stratafs_mount_op,
    .unmount = stratafs_unmount_op,
    .sync    = stratafs_sync_op,
    .statfs  = NULL,
};

static vfs_filesystem_t g_stratafs_fs = {
    .name      = "stratafs",
    .mount_ops = &g_stratafs_mount_ops,
    .next      = NULL,
};

int stratafs_init(void) {
    return vfs_register_fs(&g_stratafs_fs);
}

bool stratafs_is_mounted(void) {
    return g_stratafs_active;
}
