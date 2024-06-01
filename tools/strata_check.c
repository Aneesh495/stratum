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
#define STRATAFS_NIND_BLOCKS        (STRATAFS_BLOCK_SIZE / sizeof(uint32_t))
#define STRATAFS_ADDRESSABLE        (STRATAFS_NDIR_BLOCKS + STRATAFS_NIND_BLOCKS)
#define STRATAFS_NAME_LIMIT         63
#define STRATAFS_MAX_INODES         2048
#define MODE_TYPE                  0170000
#define MODE_DIR                   0040000
#define MODE_FILE                  0100000
#define MODE_LINK                  0120000

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
    uint32_t generation;
    uint32_t reserved[4];
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

typedef struct {
    uint32_t parent, dotdot, child_dirs, references;
    bool dot, dotdot_seen;
} namespace_entry_t;

typedef struct {
    char name[STRATAFS_NAME_LIMIT + 1];
} directory_name_t;

static int compare_names(const void *lhs, const void *rhs) {
    const directory_name_t *a = lhs, *b = rhs;
    return strcmp(a->name, b->name);
}

static bool scan_directory_block(FILE *fp, uint32_t block, uint32_t ino, uint32_t limit,
                                 const uint8_t *allocated, const stratafs_disk_inode_t *inodes,
                                 namespace_entry_t *nodes, directory_name_t *names, size_t *name_count) {
    stratafs_disk_dirent_t entries[STRATAFS_BLOCK_SIZE / sizeof(stratafs_disk_dirent_t)];
    if (!read_block(fp, block, entries)) return false;
    for (size_t i = 0; i < sizeof(entries) / sizeof(entries[0]); i++) {
        const stratafs_disk_dirent_t *entry = &entries[i];
        uint32_t target = entry->inode_num;
        if (!target) continue;
        if (entry->record_len != sizeof(*entry) || !entry->name_len ||
            entry->name_len > STRATAFS_NAME_LIMIT ||
            memchr(entry->name, 0, entry->name_len) ||
            memchr(entry->name, '/', entry->name_len) ||
            target >= limit || !bitmap_get(allocated, target)) {
            fprintf(stderr, "Error: malformed directory entry in inode %u, block %u, slot %zu.\n", ino, block, i);
            return false;
        }
        uint16_t type = inodes[target].mode & MODE_TYPE;
        if (!((type == MODE_FILE && entry->file_type == 1) ||
              (type == MODE_DIR && entry->file_type == 2) ||
              (type == MODE_LINK && entry->file_type == 3))) {
            fprintf(stderr, "Error: directory inode %u has a type mismatch for inode %u.\n", ino, target);
            return false;
        }

        char *name = names[(*name_count)++].name;
        memcpy(name, entry->name, entry->name_len);
        name[entry->name_len] = 0;
        if (!strcmp(name, ".")) {
            if (nodes[ino].dot || target != ino) return false;
            nodes[ino].dot = true;
        } else if (!strcmp(name, "..")) {
            if (nodes[ino].dotdot_seen || type != MODE_DIR) return false;
            nodes[ino].dotdot_seen = true;
            nodes[ino].dotdot = target;
        } else {
            if (type == MODE_DIR && (nodes[target].parent || target == STRATAFS_ROOT_INO)) {
                fprintf(stderr, "Error: directory inode %u has multiple parents or is the root.\n", target);
                return false;
            }
            nodes[target].parent = ino;
            nodes[target].references++;
            if (type == MODE_DIR) nodes[ino].child_dirs++;
        }
    }
    return true;
}

static bool mark_data(const stratafs_sb_t *sb, uint8_t *used, uint32_t inode, uint32_t block) {
    if (block < sb->data_start_block || block >= sb->total_blocks || bitmap_get(used, block)) {
        fprintf(stderr, "Error: inode %u references invalid or multiply owned block %u.\n", inode, block);
        return false;
    }
    bitmap_set(used, block);
    return true;
}

static bool scan_inode(FILE *fp, const stratafs_sb_t *sb, uint32_t ino,
                       const stratafs_disk_inode_t *inodes, const uint8_t *allocated,
                       uint8_t *blocks, namespace_entry_t *nodes, directory_name_t *names) {
    const stratafs_disk_inode_t *inode = &inodes[ino];
    uint16_t type = inode->mode & MODE_TYPE;
    bool directory = type == MODE_DIR;
    if ((!directory && type != MODE_FILE && type != MODE_LINK) || (ino == STRATAFS_ROOT_INO && !directory) ||
        !inode->nlink || !inode->generation || inode->double_indirect ||
        inode->size > STRATAFS_ADDRESSABLE * STRATAFS_BLOCK_SIZE ||
        (directory && (!inode->size || inode->size % STRATAFS_BLOCK_SIZE))) {
        fprintf(stderr, "Error: inode %u has invalid metadata.\n", ino);
        return false;
    }

    if (type == MODE_LINK) {
        uint8_t target[STRATAFS_BLOCK_SIZE];
        if (inode->size == 0 || inode->size >= 256 || inode->nlink != 1 ||
            inode->direct[0] < sb->data_start_block || inode->direct[0] >= sb->total_blocks ||
            !read_block(fp, inode->direct[0], target) || target[0] != '/' || memchr(target, 0, inode->size)) {
            fprintf(stderr, "Error: inode %u has an invalid symbolic link.\n", ino);
            return false;
        }
    }

    uint32_t logical = (uint32_t)((inode->size + STRATAFS_BLOCK_SIZE - 1) / STRATAFS_BLOCK_SIZE);
    uint32_t addresses[STRATAFS_ADDRESSABLE] = {0};
    memcpy(addresses, inode->direct, sizeof(inode->direct));
    unsigned owned = 0;
    if (inode->indirect) {
        if (logical <= STRATAFS_NDIR_BLOCKS || !mark_data(sb, blocks, ino, inode->indirect) ||
            !read_block(fp, inode->indirect, addresses + STRATAFS_NDIR_BLOCKS)) return false;
        owned++;
    }

    size_t name_count = 0;
    for (uint32_t i = 0; i < STRATAFS_ADDRESSABLE; i++) {
        uint32_t block = addresses[i];
        if (!block && (!directory || i >= logical)) continue;
        if (!block || i >= logical || !mark_data(sb, blocks, ino, block)) {
            fprintf(stderr, "Error: inode %u has an invalid block at logical index %u.\n", ino, i);
            return false;
        }
        owned++;
        if (directory && !scan_directory_block(fp, block, ino, sb->total_inodes, allocated,
                                                inodes, nodes, names, &name_count)) return false;
    }

    if (inode->blocks != (uint64_t)owned * (STRATAFS_BLOCK_SIZE / 512)) {
        fprintf(stderr, "Error: inode %u block count disagrees with its pointers.\n", ino);
        return false;
    }
    if (directory) {
        qsort(names, name_count, sizeof(*names), compare_names);
        for (size_t i = 1; i < name_count; i++) {
            if (compare_names(&names[i - 1], &names[i]) == 0) {
                fprintf(stderr, "Error: duplicate name in directory inode %u.\n", ino);
                return false;
            }
        }
    }
    return true;
}

static bool check_namespace(const stratafs_disk_inode_t *inodes, const uint8_t *allocated,
                            const namespace_entry_t *nodes, uint32_t limit) {
    for (uint32_t ino = 1; ino < limit; ino++) {
        if (!bitmap_get(allocated, ino)) continue;
        const namespace_entry_t *node = &nodes[ino];
        bool root = ino == STRATAFS_ROOT_INO;
        uint32_t parent = root ? STRATAFS_ROOT_INO : node->parent;
        bool directory = (inodes[ino].mode & MODE_TYPE) == MODE_DIR;
        if (!parent || (directory && (!node->dot || !node->dotdot_seen || node->dotdot != parent)) ||
            inodes[ino].nlink != (directory ? 2 + node->child_dirs : node->references)) {
            fprintf(stderr, "Error: inode %u has an invalid namespace parent or link count.\n", ino);
            return false;
        }
        for (uint32_t hops = 0; parent && parent != STRATAFS_ROOT_INO && hops < limit; hops++) {
            parent = nodes[parent].parent;
        }
        if (parent != STRATAFS_ROOT_INO) {
            fprintf(stderr, "Error: inode %u is in a namespace cycle or disconnected subtree.\n", ino);
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
        image_bytes % STRATAFS_BLOCK_SIZE ||
        sb.total_blocks != (uint64_t)image_bytes / STRATAFS_BLOCK_SIZE ||
        sb.total_inodes != STRATAFS_MAX_INODES || sb.root_inode != STRATAFS_ROOT_INO ||
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

    stratafs_disk_inode_t *inodes = calloc(sb.total_inodes, sizeof(*inodes));
    namespace_entry_t *nodes = calloc(sb.total_inodes, sizeof(*nodes));
    size_t name_slots = STRATAFS_ADDRESSABLE * (STRATAFS_BLOCK_SIZE / sizeof(stratafs_disk_dirent_t));
    directory_name_t *names = calloc(name_slots, sizeof(*names));
    if (!inodes || !nodes || !names) {
        fprintf(stderr, "Error: cannot allocate checker workspace.\n");
        has_error = true;
    }
    for (uint32_t i = 0; !has_error && i < sb.inode_table_blocks; i++) {
        if (!read_block(fp, sb.inode_table_block + i, inodes + i * STRATAFS_INODES_PER_BLOCK)) {
            fprintf(stderr, "Error: cannot read inode table block %u.\n", i);
            has_error = true;
        }
    }
    for (uint32_t ino = 1; !has_error && ino < sb.total_inodes; ino++) {
        if (!bitmap_get(disk_i_bitmap, ino)) continue;
        active_inodes++;
        if ((inodes[ino].mode & MODE_TYPE) == MODE_DIR) dir_count++;
        else reg_count++;
        if (!scan_inode(fp, &sb, ino, inodes, disk_i_bitmap, reach_b_bitmap, nodes, names)) has_error = true;
    }
    if (!has_error) has_error = !check_namespace(inodes, disk_i_bitmap, nodes, sb.total_inodes);
    free(names);
    free(nodes);
    free(inodes);
    if (has_error) {
        fprintf(stderr, "Error: inode or directory namespace verification failed.\n");
        fclose(fp);
        return 1;
    }
    printf("[OK] Namespace: %u reachable inodes (%u directories, %u files)\n",
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
