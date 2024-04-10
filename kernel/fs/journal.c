#include <kernel/journal.h>
#include <kernel/slab.h>
#include <kernel/kernel.h>
#include <kernel/string.h>
#include <shared/errno.h>

#define JOURNAL_SECTORS (JOURNAL_BLOCK_SIZE / VIRTIO_BLK_SECTOR_SIZE)
#define JOURNAL_CHECKSUMMED 1U

_Static_assert(sizeof(journal_sb_t) == JOURNAL_BLOCK_SIZE, "journal superblock layout");
_Static_assert(sizeof(journal_desc_block_t) == JOURNAL_BLOCK_SIZE, "journal descriptor layout");
_Static_assert(sizeof(journal_commit_block_t) == JOURNAL_BLOCK_SIZE, "journal commit layout");

typedef enum {
    CRASH_DISABLED,
    CRASH_BEFORE_DESCRIPTOR,
    CRASH_AFTER_DESCRIPTOR,
    CRASH_FIRST_PAYLOAD,
    CRASH_PAYLOAD_FLUSHED,
    CRASH_COMMIT_WRITTEN,
    CRASH_COMMIT_FLUSHED,
    CRASH_PUBLISHED,
    CRASH_FIRST_HOME,
    CRASH_HOME_FLUSHED,
    CRASH_CHECKPOINTED,
} crash_stage_t;

static crash_stage_t crash_stage;
static const char *const crash_labels[] = {
    [CRASH_BEFORE_DESCRIPTOR] = "before-descriptor",
    [CRASH_AFTER_DESCRIPTOR] = "after-descriptor",
    [CRASH_FIRST_PAYLOAD] = "first-payload",
    [CRASH_PAYLOAD_FLUSHED] = "payload-flushed",
    [CRASH_COMMIT_WRITTEN] = "commit-written",
    [CRASH_COMMIT_FLUSHED] = "commit-flushed",
    [CRASH_PUBLISHED] = "published",
    [CRASH_FIRST_HOME] = "first-home",
    [CRASH_HOME_FLUSHED] = "home-flushed",
    [CRASH_CHECKPOINTED] = "checkpointed",
};

void journal_test_configure(const char *cmdline) {
    static const char prefix[] = "crash=wal-";
    if (strncmp(cmdline, prefix, sizeof(prefix) - 1) != 0) return;
    const char *stage = cmdline + sizeof(prefix) - 1;
    for (unsigned i = 1; i < sizeof(crash_labels) / sizeof(crash_labels[0]); i++) {
        size_t length = strlen(crash_labels[i]);
        if (strncmp(stage, crash_labels[i], length) == 0 &&
            (stage[length] == 0 || stage[length] == '\n')) {
            crash_stage = (crash_stage_t)i;
            return;
        }
    }
}

static void crash_probe(crash_stage_t stage) {
    if (crash_stage != stage) return;
    kprintf("[TEST] WAL crash stage %s.\n", crash_labels[stage]);
    cli();
    for (;;) hlt();
}

uint32_t journal_crc32(const void *data, size_t length) {
    const uint8_t *bytes = data;
    uint32_t crc = 0xFFFFFFFFU;
    while (length--) {
        crc ^= *bytes++;
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc >> 1) ^ (0xEDB88320U & (0U - (crc & 1U)));
        }
    }
    return ~crc;
}

static uint32_t payload_crc(uint32_t previous, const void *data) {
    return ((previous << 1) | (previous >> 31)) ^ journal_crc32(data, JOURNAL_BLOCK_SIZE);
}

static int read_block(virtio_blk_dev_t *dev, uint64_t block, void *buf) {
    return virtio_blk_read(dev, block * JOURNAL_SECTORS, JOURNAL_SECTORS, buf)
        == JOURNAL_BLOCK_SIZE ? 0 : -STRATUM_EIO;
}

static int write_block(virtio_blk_dev_t *dev, uint64_t block, const void *buf) {
    return virtio_blk_write(dev, block * JOURNAL_SECTORS, JOURNAL_SECTORS, buf)
        == JOURNAL_BLOCK_SIZE ? 0 : -STRATUM_EIO;
}

static uint32_t next_slot(const journal_t *j, uint32_t slot) {
    return slot + 1 == j->block_count ? 1 : slot + 1;
}

static bool valid_target(const journal_t *j, uint64_t block) {
    return block > 0 && block < j->dev->capacity_sectors / JOURNAL_SECTORS &&
        (block < j->start_block || block >= j->start_block + j->block_count);
}

static int persist_superblock(virtio_blk_dev_t *dev, uint64_t block, journal_sb_t *sb) {
    sb->flags = JOURNAL_CHECKSUMMED;
    sb->checksum = journal_crc32(sb, offsetof(journal_sb_t, checksum));
    int res = write_block(dev, block, sb);
    return res ? res : virtio_blk_flush(dev);
}

int journal_format(virtio_blk_dev_t *dev, uint64_t start_block, uint32_t block_count) {
    journal_sb_t *sb = kmalloc(sizeof(*sb));
    if (!sb) return -STRATUM_ENOMEM;
    memset(sb, 0, sizeof(*sb));
    sb->magic = JOURNAL_MAGIC_SB;
    sb->block_size = JOURNAL_BLOCK_SIZE;
    sb->total_blocks = block_count;
    sb->head_block = 1;
    sb->tail_block = 1;
    sb->sequence_num = 1;
    int res = persist_superblock(dev, start_block, sb);
    kfree(sb);
    return res;
}

static int apply(journal_t *j, const journal_tx_t *tx) {
    for (uint32_t i = 0; i < tx->num_blocks; i++) {
        int res = write_block(j->dev, tx->blocks[i].target_block, tx->blocks[i].data);
        if (res != 0) return res;
        if (i == 0) crash_probe(CRASH_FIRST_HOME);
    }
    int res = virtio_blk_flush(j->dev);
    if (res == 0) crash_probe(CRASH_HOME_FLUSHED);
    return res;
}

static int recover(journal_t *j) {
    if (j->sb.head_block == j->sb.tail_block) return 0;

    journal_tx_t *tx = kmalloc(sizeof(*tx));
    void *block = kmalloc(JOURNAL_BLOCK_SIZE);
    if (!tx || !block) {
        kfree(tx);
        kfree(block);
        return -STRATUM_ENOMEM;
    }

    int res = -STRATUM_EIO;
    uint32_t slot = j->sb.head_block;
    journal_desc_block_t *desc = block;
    if (read_block(j->dev, j->start_block + slot, desc) != 0 ||
        desc->magic != JOURNAL_MAGIC_DESC || desc->num_blocks == 0 ||
        desc->num_blocks > JOURNAL_MAX_TX_BLOCKS ||
        desc->sequence_num != j->sb.sequence_num - 1) goto out;

    uint32_t desc_crc = desc->checksum;
    desc->checksum = 0;
    if (journal_crc32(desc, sizeof(*desc)) != desc_crc) goto out;

    tx->sequence_num = desc->sequence_num;
    tx->num_blocks = desc->num_blocks;
    uint32_t crc = 0;
    for (uint32_t i = 0; i < tx->num_blocks; i++) {
        slot = next_slot(j, slot);
        uint64_t target = desc->target_blocks[i];
        if (!valid_target(j, target) || slot == j->sb.tail_block) goto out;
        tx->blocks[i].target_block = target;
        if (read_block(j->dev, j->start_block + slot, tx->blocks[i].data) != 0) goto out;
        crc = payload_crc(crc, tx->blocks[i].data);
    }

    slot = next_slot(j, slot);
    journal_commit_block_t *commit = block;
    if (next_slot(j, slot) != j->sb.tail_block ||
        read_block(j->dev, j->start_block + slot, commit) != 0 ||
        commit->magic != JOURNAL_MAGIC_COMMIT || commit->sequence_num != tx->sequence_num ||
        commit->data_crc32 != crc ||
        journal_crc32(commit, offsetof(journal_commit_block_t, checksum)) != commit->checksum) goto out;

    res = apply(j, tx);
    if (res != 0) goto out;
    j->sb.head_block = j->sb.tail_block;
    res = persist_superblock(j->dev, j->start_block, &j->sb);
    if (res == 0) kprintf("[JOURNAL] Replayed committed transaction %lu.\n", tx->sequence_num);
out:
    kfree(block);
    kfree(tx);
    return res;
}

int journal_init(virtio_blk_dev_t *dev, uint64_t start_block, uint32_t block_count, journal_t *j) {
    if (!dev || !j || block_count < JOURNAL_MAX_TX_BLOCKS + 4) return -STRATUM_EINVAL;
    uint64_t device_blocks = dev->capacity_sectors / JOURNAL_SECTORS;
    if (start_block >= device_blocks || block_count > device_blocks - start_block) return -STRATUM_EINVAL;

    memset(j, 0, sizeof(*j));
    j->dev = dev;
    j->start_block = start_block;
    j->block_count = block_count;
    journal_sb_t *sb = &j->sb;
    if (read_block(dev, start_block, sb) != 0 || sb->magic != JOURNAL_MAGIC_SB ||
        sb->block_size != JOURNAL_BLOCK_SIZE || sb->total_blocks != block_count ||
        sb->head_block == 0 || sb->head_block >= block_count ||
        sb->tail_block == 0 || sb->tail_block >= block_count || sb->sequence_num == 0) {
        return -STRATUM_EIO;
    }

    if (sb->flags == 0 && sb->head_block == sb->tail_block) {
        /* Earlier clean journals left their header checksum stale. */
        int res = persist_superblock(dev, start_block, sb);
        if (res != 0) return res;
    } else if (sb->flags != JOURNAL_CHECKSUMMED ||
               journal_crc32(sb, offsetof(journal_sb_t, checksum)) != sb->checksum) {
        return -STRATUM_EIO;
    }

    int res = recover(j);
    if (res != 0) return res;
    j->active = true;
    return 0;
}

int journal_begin(journal_t *j, journal_tx_t **out_tx) {
    if (!j->active) return -STRATUM_EIO;
    if (j->dev->read_only) return -STRATUM_EROFS;
    journal_tx_t *tx = kmalloc(sizeof(*tx));
    if (!tx) return -STRATUM_ENOMEM;
    tx->sequence_num = j->sb.sequence_num;
    tx->num_blocks = 0;
    *out_tx = tx;
    return 0;
}

void journal_abort(journal_tx_t *tx) {
    kfree(tx);
}

int journal_write_block(journal_tx_t *tx, uint64_t target_block, const void *data) {
    uint32_t i = 0;
    while (i < tx->num_blocks && tx->blocks[i].target_block != target_block) i++;
    if (i == JOURNAL_MAX_TX_BLOCKS) return -STRATUM_ENOSPC;
    tx->blocks[i].target_block = target_block;
    memcpy(tx->blocks[i].data, data, JOURNAL_BLOCK_SIZE);
    if (i == tx->num_blocks) tx->num_blocks++;
    return 0;
}

int journal_read_block(journal_tx_t *tx, uint64_t target_block, void *buf) {
    for (uint32_t i = 0; i < tx->num_blocks; i++) {
        if (tx->blocks[i].target_block == target_block) {
            memcpy(buf, tx->blocks[i].data, JOURNAL_BLOCK_SIZE);
            return 0;
        }
    }
    return -STRATUM_ENOENT;
}

int journal_commit(journal_t *j, journal_tx_t *tx) {
    if (tx->num_blocks == 0) {
        journal_abort(tx);
        return 0;
    }

    void *block = kmalloc(JOURNAL_BLOCK_SIZE);
    if (!block) {
        journal_abort(tx);
        return -STRATUM_ENOMEM;
    }

    int res = -STRATUM_EIO;
    journal_desc_block_t *desc = block;
    memset(desc, 0, sizeof(*desc));
    desc->magic = JOURNAL_MAGIC_DESC;
    desc->sequence_num = tx->sequence_num;
    desc->num_blocks = tx->num_blocks;
    for (uint32_t i = 0; i < tx->num_blocks; i++) {
        if (!valid_target(j, tx->blocks[i].target_block)) goto out;
        desc->target_blocks[i] = tx->blocks[i].target_block;
    }
    desc->checksum = journal_crc32(desc, sizeof(*desc));

    uint32_t slot = j->sb.tail_block;
    crash_probe(CRASH_BEFORE_DESCRIPTOR);
    if (write_block(j->dev, j->start_block + slot, desc) != 0) goto out;
    crash_probe(CRASH_AFTER_DESCRIPTOR);
    uint32_t crc = 0;
    for (uint32_t i = 0; i < tx->num_blocks; i++) {
        slot = next_slot(j, slot);
        if (write_block(j->dev, j->start_block + slot, tx->blocks[i].data) != 0) goto out;
        crc = payload_crc(crc, tx->blocks[i].data);
        if (i == 0) crash_probe(CRASH_FIRST_PAYLOAD);
    }
    if (virtio_blk_flush(j->dev) != 0) goto out;
    crash_probe(CRASH_PAYLOAD_FLUSHED);

    journal_commit_block_t *commit = block;
    memset(commit, 0, sizeof(*commit));
    commit->magic = JOURNAL_MAGIC_COMMIT;
    commit->sequence_num = tx->sequence_num;
    commit->data_crc32 = crc;
    commit->checksum = journal_crc32(commit, offsetof(journal_commit_block_t, checksum));
    slot = next_slot(j, slot);
    if (write_block(j->dev, j->start_block + slot, commit) != 0) goto out;
    crash_probe(CRASH_COMMIT_WRITTEN);
    if (virtio_blk_flush(j->dev) != 0) goto out;
    crash_probe(CRASH_COMMIT_FLUSHED);

    /* Publish the replay range before making any changes to home blocks. */
    j->sb.tail_block = next_slot(j, slot);
    j->sb.sequence_num++;
    if (persist_superblock(j->dev, j->start_block, &j->sb) != 0) goto out;
    crash_probe(CRASH_PUBLISHED);
    if (apply(j, tx) != 0) goto out;

    j->sb.head_block = j->sb.tail_block;
    res = persist_superblock(j->dev, j->start_block, &j->sb);
    if (res == 0) crash_probe(CRASH_CHECKPOINTED);
out:
    /* An uncertain commit must remain available for replay on next mount. */
    if (res != 0) j->active = false;
    kfree(block);
    journal_abort(tx);
    return res;
}

int journal_checkpoint(journal_t *j) {
    return j->active ? virtio_blk_flush(j->dev) : -STRATUM_EIO;
}
