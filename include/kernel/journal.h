#ifndef STRATUM_KERNEL_JOURNAL_H
#define STRATUM_KERNEL_JOURNAL_H

#include <kernel/types.h>
#include <kernel/virtio_blk.h>
#include <kernel/spinlock.h>

#define JOURNAL_MAGIC_SB        0x4A4F55524E414C31ULL /* "JOURNAL1" */
#define JOURNAL_MAGIC_DESC      0x54584445534331ULL   /* "TXDESC1"  */
#define JOURNAL_MAGIC_COMMIT    0x5458434F4D4D31ULL   /* "TXCOMM1"  */

#define JOURNAL_BLOCK_SIZE      4096
#define JOURNAL_MAX_TX_BLOCKS   64

typedef struct journal_sb {
    uint64_t magic;
    uint32_t block_size;
    uint32_t total_blocks;
    uint32_t head_block;        /* First uncheckpointed transaction */
    uint32_t tail_block;        /* Next write position in circular log */
    uint64_t sequence_num;      /* Monotonic sequence counter */
    uint32_t flags;
    uint32_t checksum;
    uint8_t  padding[4056];
} __attribute__((packed)) journal_sb_t;

typedef struct journal_desc_block {
    uint64_t magic;
    uint64_t sequence_num;
    uint32_t num_blocks;
    uint32_t checksum;
    uint64_t target_blocks[509]; /* Disk block numbers for payload blocks */
} __attribute__((packed)) journal_desc_block_t;

typedef struct journal_commit_block {
    uint64_t magic;
    uint64_t sequence_num;
    uint64_t timestamp;
    uint32_t data_crc32;
    uint32_t checksum;
    uint8_t  padding[4064];
} __attribute__((packed)) journal_commit_block_t;

typedef struct journal_dirty_block {
    uint64_t target_block;
    uint8_t  data[JOURNAL_BLOCK_SIZE];
} journal_dirty_block_t;

typedef struct journal_tx {
    uint64_t                sequence_num;
    uint32_t                num_blocks;
    journal_dirty_block_t   blocks[JOURNAL_MAX_TX_BLOCKS];
} journal_tx_t;

typedef struct journal {
    virtio_blk_dev_t       *dev;
    uint64_t                start_block;  /* Physical block offset on disk */
    uint32_t                block_count;  /* Total blocks allocated to journal */
    journal_sb_t            sb;
    bool                    active;
} journal_t;

/* Checksum calculation */
uint32_t journal_crc32(const void *data, size_t length);

/* Journal calls are serialized by the filesystem. */
void journal_test_configure(const char *cmdline);
int  journal_format(virtio_blk_dev_t *dev, uint64_t start_block, uint32_t block_count);
int  journal_init(virtio_blk_dev_t *dev, uint64_t start_block, uint32_t block_count, journal_t *j);
int  journal_begin(journal_t *j, journal_tx_t **out_tx);
int  journal_write_block(journal_tx_t *tx, uint64_t target_block, const void *data);
int  journal_read_block(journal_tx_t *tx, uint64_t target_block, void *buf);
int  journal_commit(journal_t *j, journal_tx_t *tx);
int  journal_checkpoint(journal_t *j);
void journal_abort(journal_tx_t *tx);

#endif /* STRATUM_KERNEL_JOURNAL_H */
