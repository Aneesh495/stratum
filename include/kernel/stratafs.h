#ifndef STRATUM_KERNEL_STRATAFS_H
#define STRATUM_KERNEL_STRATAFS_H

#include <kernel/types.h>
#include <kernel/vfs.h>
#include <kernel/stratafs_dirent.h>
#include <kernel/journal.h>
#include <kernel/virtio_blk.h>
#include <kernel/spinlock.h>

#define STRATAFS_MAGIC              0x5354524154414653ULL /* "STRATAFS" */
#define STRATAFS_VERSION            1
#define STRATAFS_BLOCK_SIZE         4096
#define STRATAFS_SECTORS_PER_BLOCK  (STRATAFS_BLOCK_SIZE / VIRTIO_BLK_SECTOR_SIZE)
#define STRATAFS_INODE_SIZE         128
#define STRATAFS_INODES_PER_BLOCK   (STRATAFS_BLOCK_SIZE / STRATAFS_INODE_SIZE)
#define STRATAFS_ROOT_INO           1

#define STRATAFS_NDIR_BLOCKS        12
#define STRATAFS_INDIR_BLOCKS       (STRATAFS_BLOCK_SIZE / sizeof(uint32_t))

typedef struct stratafs_sb {
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

typedef struct stratafs_disk_inode {
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

typedef struct stratafs {
    virtio_blk_dev_t       *dev;
    stratafs_sb_t           sb;
    journal_t               journal;
    spinlock_t              lock;
    uint32_t                open_count[2048];
    vfs_mount_t            *mount;
} stratafs_t;

/* StrataFS API */
int  stratafs_init(void);
bool stratafs_is_mounted(void);
int  stratafs_format(virtio_blk_dev_t *dev);
int  stratafs_mount_fs(vfs_mount_t *mnt, const char *dev_name);

int  stratafs_read_block(stratafs_t *fs, uint64_t block_num, void *buf);
int  stratafs_write_block(stratafs_t *fs, uint64_t block_num, const void *buf);

int  stratafs_alloc_block(stratafs_t *fs, journal_tx_t *tx, uint32_t *out_block);
int  stratafs_free_block(stratafs_t *fs, journal_tx_t *tx, uint32_t block);

int  stratafs_alloc_inode(stratafs_t *fs, journal_tx_t *tx, uint16_t mode, uint32_t *out_ino);
int  stratafs_free_inode(stratafs_t *fs, journal_tx_t *tx, uint32_t ino);

int  stratafs_read_inode(stratafs_t *fs, uint32_t ino, stratafs_disk_inode_t *out_inode);
int  stratafs_write_inode(stratafs_t *fs, journal_tx_t *tx, uint32_t ino, const stratafs_disk_inode_t *inode);

int64_t stratafs_read_file(stratafs_t *fs, stratafs_disk_inode_t *inode, uint64_t offset, void *buf, size_t count);
int64_t stratafs_write_file(stratafs_t *fs, journal_tx_t *tx, uint32_t ino, stratafs_disk_inode_t *inode, uint64_t offset, const void *buf, size_t count);

int  stratafs_dir_lookup(stratafs_t *fs, stratafs_disk_inode_t *dir, const char *name, uint32_t *out_ino);
int  stratafs_dir_add(stratafs_t *fs, journal_tx_t *tx, uint32_t dir_ino, stratafs_disk_inode_t *dir, const char *name, uint32_t ino, uint8_t file_type);

#endif /* STRATUM_KERNEL_STRATAFS_H */
