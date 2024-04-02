/*
 * tools/strata_check.c - Independent StrataFS Filesystem Checker (K56).
 *
 * Validates raw StrataFS disk images on the host:
 * - Superblock geometry, version, and block limits
 * - Journal superblock, circular buffer limits, and commit sequence
 * - Inode bitmap and block bitmap consistency
 * - Inode table validity, direct and indirect block pointer bounds
 * - Directory hierarchy traversal and entry integrity
 * - Cross-checks reachable data blocks against bitmap to detect leaks or double references
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <stddef.h>

#define STRATAFS_MAGIC              0x5354524154414653ULL /* "STRATAFS" */
#define STRATAFS_VERSION            1
#define STRATAFS_BLOCK_SIZE         4096
#define STRATAFS_INODE_SIZE         128
#define STRATAFS_INODES_PER_BLOCK   (STRATAFS_BLOCK_SIZE / STRATAFS_INODE_SIZE)
#define STRATAFS_ROOT_INO           1
#define STRATAFS_NDIR_BLOCKS        12

#define JOURNAL_MAGIC_SB            0x4A4F55524E414C31ULL /* "JOURNAL1" */

typedef struct {
    uint64_t magic;
    uint32_t version;
    uint32_t block_size;
    uint64_t total_blocks;
    uint64_t free_blocks;
    uint32_t total_inodes;
    uint32_t free_inodes;
    uint32_t root_inode;
    uint32_t block_bitmap_block;
    uint32_t block_bitmap_blocks;
    uint32_t inode_bitmap_block;
    uint32_t inode_bitmap_blocks;
    uint32_t inode_table_block;
    uint32_t inode_table_blocks;
    uint32_t journal_start_block;
    uint32_t journal_block_count;
    uint32_t data_start_block;
    uint32_t data_block_count;
    uint32_t mount_count;
    uint32_t clean_shutdown;
    uint32_t checksum;
    uint8_t  padding[4000];
} __attribute__((packed)) stratafs_sb_t;

typedef struct {
    uint16_t mode;
    uint16_t uid;
    uint16_t gid;
    uint16_t nlink;
    uint64_t size;
    uint64_t blocks;
    uint64_t atime;
    uint64_t mtime;
    uint64_t ctime;
    uint32_t direct[STRATAFS_NDIR_BLOCKS];
    uint32_t indirect;
    uint32_t double_indirect;
    uint32_t flags;
    uint32_t reserved[5];
} __attribute__((packed)) stratafs_disk_inode_t;

typedef struct {
    uint32_t inode_num;
    uint16_t record_len;
    uint8_t  name_len;
    uint8_t  file_type;
    char     name[248];
} __attribute__((packed)) stratafs_disk_dirent_t;

typedef struct {
    uint64_t magic;
    uint32_t block_size;
    uint32_t total_blocks;
    uint32_t head_block;
    uint32_t tail_block;
    uint64_t sequence_num;
    uint32_t flags;
    uint32_t checksum;
    uint8_t  padding[4056];
} __attribute__((packed)) journal_sb_t;

static bool read_block(FILE *fp, uint64_t block_num, void *buf) {
    if (fseek(fp, (long)(block_num * STRATAFS_BLOCK_SIZE), SEEK_SET) != 0) {
        return false;
    }
    size_t rd = fread(buf, 1, STRATAFS_BLOCK_SIZE, fp);
    return (rd == STRATAFS_BLOCK_SIZE);
}

static inline bool bitmap_get(const uint8_t *bitmap, uint32_t bit) {
    return (bitmap[bit / 8] & (1U << (bit % 8))) != 0;
}

static inline void bitmap_set(uint8_t *bitmap, uint32_t bit) {
    bitmap[bit / 8] |= (uint8_t)(1U << (bit % 8));
}

_Static_assert(sizeof(stratafs_sb_t) == STRATAFS_BLOCK_SIZE, "superblock layout");
_Static_assert(sizeof(stratafs_disk_inode_t) == STRATAFS_INODE_SIZE, "inode stride");

static uint32_t crc32(const void *data, size_t len) {
    const uint8_t *bytes = data;
    uint32_t value = UINT32_MAX;
    for (size_t i = 0; i < len; i++) {
        value ^= bytes[i];
        for (unsigned b = 0; b < 8; b++) {
            uint32_t polynomial = value & 1 ? UINT32_C(0xEDB88320) : 0;
            value = (value >> 1) ^ polynomial;
        }
    }
    return ~value;
}

static bool valid_directory_block(FILE *fp, uint32_t block, uint32_t ino, uint32_t limit,
                                  const uint8_t *inode_bitmap) {
    stratafs_disk_dirent_t entries[STRATAFS_BLOCK_SIZE / sizeof(stratafs_disk_dirent_t)];
    if (!read_block(fp, block, entries)) return false;
    for (size_t i = 0; i < sizeof(entries) / sizeof(entries[0]); i++) {
        const stratafs_disk_dirent_t *entry = &entries[i];
        if (!entry->inode_num) continue;
        if (entry->record_len != sizeof(*entry) || !entry->name_len ||
            entry->name_len >= sizeof(entry->name) ||
            memchr(entry->name, 0, entry->name_len) ||
            memchr(entry->name, '/', entry->name_len) ||
            entry->inode_num >= limit || !bitmap_get(inode_bitmap, entry->inode_num) ||
            (entry->file_type != 1 && entry->file_type != 2)) {
            fprintf(stderr, "Error: malformed directory entry in inode %u, slot %zu.\n", ino, i);
            return false;
        }
    }
    return true;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <stratafs_disk_image>\n", argv[0]);
        return 2;
    }

    const char *img_path = argv[1];
    FILE *fp = fopen(img_path, "rb");
    if (!fp) {
        fprintf(stderr, "Error: cannot open '%s': %s\n", img_path, strerror(errno));
        return 2;
    }

    printf("=== StrataFS Independent Consistency Checker (K56) ===\n");
    printf("Image: %s\n", img_path);

    /* 1. Read and verify Superblock (stored at block 1) */
    stratafs_sb_t sb;
    if (!read_block(fp, 1, &sb)) {
        fprintf(stderr, "Error: failed to read superblock from block 1.\n");
        fclose(fp);
        return 1;
    }

    uint32_t expected_crc = sb.checksum;
    sb.checksum = 0;
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return 1;
    }
    long image_bytes = ftell(fp);
    if (image_bytes < 0 || sb.magic != STRATAFS_MAGIC || sb.version != STRATAFS_VERSION ||
        sb.block_size != STRATAFS_BLOCK_SIZE || sb.total_blocks < 1024 ||
        sb.total_blocks > STRATAFS_BLOCK_SIZE * 8U ||
        sb.total_blocks != (uint64_t)image_bytes / STRATAFS_BLOCK_SIZE ||
        sb.total_inodes != 2048 || sb.root_inode != STRATAFS_ROOT_INO ||
        sb.block_bitmap_block != 2 || sb.block_bitmap_blocks != 1 ||
        sb.inode_bitmap_block != 3 || sb.inode_bitmap_blocks != 1 ||
        sb.inode_table_block != 4 || sb.inode_table_blocks != 64 ||
        sb.journal_start_block != 68 || sb.journal_block_count != 512 ||
        sb.data_start_block != 580 || sb.data_block_count != sb.total_blocks - 580 ||
        expected_crc != crc32(&sb, offsetof(stratafs_sb_t, checksum) + sizeof(sb.checksum))) {
        fprintf(stderr, "Error: invalid StrataFS superblock geometry or checksum.\n");
        fclose(fp);
        return 1;
    }

    printf("[OK] Superblock valid: total_blocks=%llu, free_blocks=%llu, total_inodes=%u, free_inodes=%u\n",
           (unsigned long long)sb.total_blocks, (unsigned long long)sb.free_blocks,
           sb.total_inodes, sb.free_inodes);

    /* 2. Read and verify Journal Superblock */
    journal_sb_t jsb;
    if (!read_block(fp, sb.journal_start_block, &jsb)) {
        fprintf(stderr, "Error: failed to read journal superblock at block %u\n", sb.journal_start_block);
        fclose(fp);
        return 1;
    }

    if (jsb.magic != JOURNAL_MAGIC_SB || jsb.block_size != STRATAFS_BLOCK_SIZE ||
        jsb.total_blocks != sb.journal_block_count ||
        jsb.head_block == 0 || jsb.head_block >= jsb.total_blocks ||
        jsb.tail_block != jsb.head_block || jsb.sequence_num == 0 || jsb.flags != 1 ||
        jsb.checksum != crc32(&jsb, offsetof(journal_sb_t, checksum))) {
        fprintf(stderr, "Error: invalid or uncheckpointed journal. Inspect after successful recovery.\n");
        fclose(fp);
        return 1;
    }
    printf("[OK] Journal valid: head=%u, tail=%u, seq=%llu, total_blocks=%u\n",
           jsb.head_block, jsb.tail_block, (unsigned long long)jsb.sequence_num, jsb.total_blocks);

    uint8_t disk_b_bitmap[STRATAFS_BLOCK_SIZE];
    uint8_t disk_i_bitmap[STRATAFS_BLOCK_SIZE];
    uint8_t reach_b_bitmap[STRATAFS_BLOCK_SIZE] = {0};
    if (!read_block(fp, sb.block_bitmap_block, disk_b_bitmap) ||
        !read_block(fp, sb.inode_bitmap_block, disk_i_bitmap)) {
        fprintf(stderr, "Error: cannot read allocation bitmaps.\n");
        fclose(fp);
        return 1;
    }

    /* Mark metadata blocks as reachable */
    bitmap_set(reach_b_bitmap, 0); /* Reserved boot block */
    bitmap_set(reach_b_bitmap, 1); /* Superblock */
    for (uint32_t i = 0; i < sb.block_bitmap_blocks; i++) bitmap_set(reach_b_bitmap, sb.block_bitmap_block + i);
    for (uint32_t i = 0; i < sb.inode_bitmap_blocks; i++) bitmap_set(reach_b_bitmap, sb.inode_bitmap_block + i);
    for (uint32_t i = 0; i < sb.inode_table_blocks; i++) bitmap_set(reach_b_bitmap, sb.inode_table_block + i);
    for (uint32_t i = 0; i < sb.journal_block_count; i++) bitmap_set(reach_b_bitmap, sb.journal_start_block + i);

    /* 4. Scan Inodes and Traverse Block Pointers */
    uint32_t active_inodes = 0;
    uint32_t dir_count = 0;
    uint32_t reg_count = 0;
    bool has_error = !bitmap_get(disk_i_bitmap, 0) || !bitmap_get(disk_i_bitmap, STRATAFS_ROOT_INO);

    uint8_t inode_buf[STRATAFS_BLOCK_SIZE];

    for (uint32_t ino = 1; ino < sb.total_inodes; ino++) {
        bool is_alloc = bitmap_get(disk_i_bitmap, ino);
        if (!is_alloc) continue;

        active_inodes++;

        uint32_t block_idx = sb.inode_table_block + (ino / STRATAFS_INODES_PER_BLOCK);
        uint32_t offset = (ino % STRATAFS_INODES_PER_BLOCK) * STRATAFS_INODE_SIZE;

        if (!read_block(fp, block_idx, inode_buf)) {
            fprintf(stderr, "Error: failed to read inode table block %u for ino %u\n", block_idx, ino);
            has_error = true;
            break;
        }

        stratafs_disk_inode_t *din = (stratafs_disk_inode_t *)(inode_buf + offset);
        uint16_t type = din->mode & 0170000;
        if (type == 0040000) dir_count++;
        else if (type == 0100000) reg_count++;
        if ((type != 0040000 && type != 0100000) ||
            (ino == STRATAFS_ROOT_INO && type != 0040000) ||
            !din->nlink || din->double_indirect) {
            fprintf(stderr, "Error: inode %u has invalid type, link count or addressing.\n", ino);
            has_error = true;
        }

        /* Check direct block pointers */
        for (int d = 0; d < STRATAFS_NDIR_BLOCKS; d++) {
            uint32_t bnum = din->direct[d];
            if (bnum == 0) continue;

            if (bnum < sb.data_start_block || bnum >= sb.total_blocks) {
                fprintf(stderr, "Error: inode %u direct block %d out of bounds: %u\n", ino, d, bnum);
                has_error = true;
                continue;
            }

            if (bitmap_get(reach_b_bitmap, bnum)) {
                fprintf(stderr, "Error: block %u multiply referenced by inode %u\n", bnum, ino);
                has_error = true;
            }
            bitmap_set(reach_b_bitmap, bnum);

            if (type == 0040000 && !valid_directory_block(fp, bnum, ino, sb.total_inodes, disk_i_bitmap)) {
                has_error = true;
            }
        }

        /* Check single indirect block pointer */
        if (din->indirect != 0) {
            uint32_t iblk = din->indirect;
            if (iblk < sb.data_start_block || iblk >= sb.total_blocks) {
                fprintf(stderr, "Error: inode %u indirect block out of bounds: %u\n", ino, iblk);
                has_error = true;
            } else {
                if (bitmap_get(reach_b_bitmap, iblk)) {
                    fprintf(stderr, "Error: indirect block %u multiply referenced.\n", iblk);
                    has_error = true;
                }
                bitmap_set(reach_b_bitmap, iblk);
                uint32_t indir_ptrs[STRATAFS_BLOCK_SIZE / sizeof(uint32_t)];
                if (read_block(fp, iblk, indir_ptrs)) {
                    for (size_t p = 0; p < (STRATAFS_BLOCK_SIZE / sizeof(uint32_t)); p++) {
                        uint32_t pb = indir_ptrs[p];
                        if (pb == 0) continue;
                        if (pb < sb.data_start_block || pb >= sb.total_blocks) {
                            fprintf(stderr, "Error: inode %u indirect child block out of bounds: %u\n", ino, pb);
                            has_error = true;
                            continue;
                        }
                        if (bitmap_get(reach_b_bitmap, pb)) {
                            fprintf(stderr, "Error: block %u multiply referenced in indirect block of ino %u\n", pb, ino);
                            has_error = true;
                        }
                        bitmap_set(reach_b_bitmap, pb);
                        if (type == 0040000 && !valid_directory_block(fp, pb, ino, sb.total_inodes, disk_i_bitmap)) {
                            has_error = true;
                        }
                    }
                } else {
                    has_error = true;
                }
            }
        }
    }

    printf("[OK] Inode table scan: %u active inodes (%u directories, %u files)\n",
           active_inodes, dir_count, reg_count);

    /* 5. Cross-check Reachable Blocks with On-Disk Block Bitmap */
    uint32_t leaks = 0;
    uint32_t unrecorded = 0;
    uint32_t allocated = 0;

    for (uint32_t b = 0; b < sb.total_blocks; b++) {
        bool on_disk = bitmap_get(disk_b_bitmap, b);
        bool reachable = bitmap_get(reach_b_bitmap, b);
        if (on_disk) allocated++;

        if (on_disk && !reachable) {
            leaks++;
        } else if (!on_disk && reachable) {
            unrecorded++;
        }
    }

    if (leaks > 0) {
        fprintf(stderr, "Error: %u allocated blocks lack an owner.\n", leaks);
        has_error = true;
    }
    if (unrecorded > 0) {
        fprintf(stderr, "Error: %u reachable blocks are NOT marked allocated in block bitmap!\n", unrecorded);
        has_error = true;
    }

    if (sb.free_blocks != sb.total_blocks - allocated ||
        sb.free_inodes != sb.total_inodes - active_inodes - 1) {
        fprintf(stderr, "Error: free counts disagree with allocation bitmaps.\n");
        has_error = true;
    }
    fclose(fp);

    if (has_error) {
        printf("[FAILED] Filesystem integrity verification failed.\n");
        return 1;
    }

    printf("[PASSED] StrataFS filesystem structure is consistent and verified.\n");
    return 0;
}
