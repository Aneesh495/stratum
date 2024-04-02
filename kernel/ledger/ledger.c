#include <kernel/ledger.h>
#include <kernel/kernel.h>
#include <kernel/vfs.h>
#include <kernel/string.h>
#include <kernel/socket.h>
#include <kernel/tcp.h>
#include <shared/errno.h>

static ledger_state_t g_ledger;
static const uint8_t g_node_secret_key[32] = {
    0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF,
    0xFE, 0xDC, 0xBA, 0x98, 0x76, 0x54, 0x32, 0x10,
    0x55, 0xAA, 0x55, 0xAA, 0x33, 0xCC, 0x33, 0xCC,
    0x0F, 0xF0, 0x0F, 0xF0, 0x77, 0x88, 0x99, 0xAA
};

void ledger_hash_tx(ledger_tx_t *tx) {
    if (!tx) return;
    sha256_ctx_t ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, &tx->tx_id, sizeof(tx->tx_id));
    sha256_update(&ctx, &tx->sender_id, sizeof(tx->sender_id));
    sha256_update(&ctx, &tx->recipient_id, sizeof(tx->recipient_id));
    sha256_update(&ctx, &tx->amount, sizeof(tx->amount));
    sha256_update(&ctx, &tx->timestamp, sizeof(tx->timestamp));
    sha256_update(&ctx, &tx->payload_len, sizeof(tx->payload_len));
    if (tx->payload_len > 0) {
        sha256_update(&ctx, tx->payload, tx->payload_len);
    }
    sha256_final(&ctx, tx->tx_hash);

    hmac_sha256(g_node_secret_key, sizeof(g_node_secret_key),
                tx->tx_hash, SHA256_DIGEST_SIZE, tx->signature);
}

static void compute_block_hash(ledger_block_t *block) {
    sha256_ctx_t ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, &block->magic, sizeof(block->magic));
    sha256_update(&ctx, &block->version, sizeof(block->version));
    sha256_update(&ctx, &block->block_index, sizeof(block->block_index));
    sha256_update(&ctx, &block->timestamp, sizeof(block->timestamp));
    sha256_update(&ctx, &block->nonce, sizeof(block->nonce));
    sha256_update(&ctx, block->prev_hash, SHA256_DIGEST_SIZE);
    sha256_update(&ctx, block->merkle_root, SHA256_DIGEST_SIZE);
    sha256_update(&ctx, &block->tx_count, sizeof(block->tx_count));
    sha256_final(&ctx, block->block_hash);
}

int ledger_validate_block(const ledger_block_t *block, const ledger_block_t *prev_block) {
    if (!block) return -STRATUM_EINVAL;
    if (block->magic != LEDGER_MAGIC) return -STRATUM_EINVAL;
    if (block->version != LEDGER_VERSION) return -STRATUM_EINVAL;
    if (block->tx_count > LEDGER_MAX_TX_PER_BLOCK) return -STRATUM_EINVAL;

    if (block->block_index == 0) {
        /* Genesis block */
        for (int i = 0; i < SHA256_DIGEST_SIZE; i++) {
            if (block->prev_hash[i] != 0) return -STRATUM_EINVAL;
        }
    } else {
        if (!prev_block) return -STRATUM_EINVAL;
        if (block->block_index != prev_block->block_index + 1) return -STRATUM_EINVAL;
        if (memcmp(block->prev_hash, prev_block->block_hash, SHA256_DIGEST_SIZE) != 0) {
            return -STRATUM_EINVAL;
        }
    }

    /* Verify Merkle Root */
    uint8_t tx_hashes[LEDGER_MAX_TX_PER_BLOCK][SHA256_DIGEST_SIZE];
    for (uint32_t i = 0; i < block->tx_count; i++) {
        ledger_tx_t temp_tx = block->txs[i];
        ledger_hash_tx(&temp_tx);
        if (memcmp(temp_tx.tx_hash, block->txs[i].tx_hash, SHA256_DIGEST_SIZE) != 0) {
            return -STRATUM_EINVAL; /* Corrupt transaction hash */
        }
        memcpy(tx_hashes[i], block->txs[i].tx_hash, SHA256_DIGEST_SIZE);
    }

    uint8_t calc_merkle[SHA256_DIGEST_SIZE];
    sha256_merkle_root(tx_hashes, block->tx_count, calc_merkle);
    if (memcmp(calc_merkle, block->merkle_root, SHA256_DIGEST_SIZE) != 0) {
        return -STRATUM_EINVAL; /* Merkle mismatch */
    }

    /* Verify Block Hash */
    ledger_block_t copy = *block;
    compute_block_hash(&copy);
    if (memcmp(copy.block_hash, block->block_hash, SHA256_DIGEST_SIZE) != 0) {
        return -STRATUM_EINVAL; /* Hash mismatch */
    }

    return 0;
}

int ledger_init(const char *storage_path) {
    spin_lock_init(&g_ledger.lock);
    uint64_t flags;
    spin_lock_irqsave(&g_ledger.lock, &flags);

    g_ledger.initialized = true;
    g_ledger.chain_height = 0;
    g_ledger.total_txs = 0;
    g_ledger.mempool_count = 0;
    memset(g_ledger.chain, 0, sizeof(g_ledger.chain));
    memset(g_ledger.mempool, 0, sizeof(g_ledger.mempool));

    if (storage_path) {
        strncpy(g_ledger.storage_path, storage_path, sizeof(g_ledger.storage_path) - 1);
    } else {
        strcpy(g_ledger.storage_path, LEDGER_PATH);
    }

    /* Create Genesis Block */
    ledger_block_t *genesis = &g_ledger.chain[0];
    genesis->magic = LEDGER_MAGIC;
    genesis->version = LEDGER_VERSION;
    genesis->block_index = 0;
    genesis->timestamp = 1000000;
    genesis->nonce = 0x12345;
    memset(genesis->prev_hash, 0, SHA256_DIGEST_SIZE);
    genesis->tx_count = 0;
    sha256_merkle_root(NULL, 0, genesis->merkle_root);
    compute_block_hash(genesis);

    g_ledger.chain_height = 1;
    spin_unlock_irqrestore(&g_ledger.lock, flags);

    kprintf("[LEDGER] Initialized distributed durable ledger. Genesis hash: %02x%02x%02x%02x...\n",
            genesis->block_hash[0], genesis->block_hash[1],
            genesis->block_hash[2], genesis->block_hash[3]);
    return 0;
}

int ledger_submit_tx(uint64_t sender, uint64_t recipient, uint64_t amount,
                      const void *payload, uint32_t payload_len) {
    uint64_t flags;
    spin_lock_irqsave(&g_ledger.lock, &flags);

    if (g_ledger.mempool_count >= LEDGER_MEMPOOL_CAPACITY) {
        spin_unlock_irqrestore(&g_ledger.lock, flags);
        return -STRATUM_ENOMEM;
    }

    ledger_tx_t *tx = &g_ledger.mempool[g_ledger.mempool_count++];
    memset(tx, 0, sizeof(ledger_tx_t));
    tx->tx_id = g_ledger.total_txs + g_ledger.mempool_count;
    tx->sender_id = sender;
    tx->recipient_id = recipient;
    tx->amount = amount;
    tx->timestamp = 1000000 + g_ledger.total_txs;
    tx->payload_len = (payload_len > LEDGER_MAX_PAYLOAD) ? LEDGER_MAX_PAYLOAD : payload_len;
    if (payload && tx->payload_len > 0) {
        memcpy(tx->payload, payload, tx->payload_len);
    }

    ledger_hash_tx(tx);

    spin_unlock_irqrestore(&g_ledger.lock, flags);
    return 0;
}

int ledger_create_block(uint64_t timestamp, ledger_block_t *out_block) {
    if (!out_block) return -STRATUM_EINVAL;
    uint64_t flags;
    spin_lock_irqsave(&g_ledger.lock, &flags);

    if (g_ledger.chain_height >= LEDGER_CHAIN_CAPACITY) {
        spin_unlock_irqrestore(&g_ledger.lock, flags);
        return -STRATUM_ENOSPC;
    }

    memset(out_block, 0, sizeof(ledger_block_t));
    out_block->magic = LEDGER_MAGIC;
    out_block->version = LEDGER_VERSION;
    out_block->block_index = g_ledger.chain_height;
    out_block->timestamp = timestamp;
    out_block->nonce = 42;

    const ledger_block_t *prev = &g_ledger.chain[g_ledger.chain_height - 1];
    memcpy(out_block->prev_hash, prev->block_hash, SHA256_DIGEST_SIZE);

    uint32_t count = g_ledger.mempool_count;
    if (count > LEDGER_MAX_TX_PER_BLOCK) {
        count = LEDGER_MAX_TX_PER_BLOCK;
    }
    out_block->tx_count = count;

    uint8_t tx_hashes[LEDGER_MAX_TX_PER_BLOCK][SHA256_DIGEST_SIZE];
    for (uint32_t i = 0; i < count; i++) {
        out_block->txs[i] = g_ledger.mempool[i];
        memcpy(tx_hashes[i], g_ledger.mempool[i].tx_hash, SHA256_DIGEST_SIZE);
    }

    sha256_merkle_root(tx_hashes, count, out_block->merkle_root);
    compute_block_hash(out_block);

    spin_unlock_irqrestore(&g_ledger.lock, flags);
    return 0;
}

int ledger_append_block(const ledger_block_t *block) {
    if (!block) return -STRATUM_EINVAL;
    uint64_t flags;
    spin_lock_irqsave(&g_ledger.lock, &flags);

    if (g_ledger.chain_height >= LEDGER_CHAIN_CAPACITY) {
        spin_unlock_irqrestore(&g_ledger.lock, flags);
        return -STRATUM_ENOSPC;
    }

    const ledger_block_t *prev = (g_ledger.chain_height > 0) ?
                                  &g_ledger.chain[g_ledger.chain_height - 1] : NULL;
    int val = ledger_validate_block(block, prev);
    if (val != 0) {
        spin_unlock_irqrestore(&g_ledger.lock, flags);
        return val;
    }

    g_ledger.chain[g_ledger.chain_height] = *block;
    g_ledger.chain_height++;
    g_ledger.total_txs += block->tx_count;

    /* Drain committed transactions from mempool */
    if (block->tx_count >= g_ledger.mempool_count) {
        g_ledger.mempool_count = 0;
    } else {
        uint32_t remaining = g_ledger.mempool_count - block->tx_count;
        memmove(&g_ledger.mempool[0], &g_ledger.mempool[block->tx_count],
                remaining * sizeof(ledger_tx_t));
        g_ledger.mempool_count = remaining;
    }

    spin_unlock_irqrestore(&g_ledger.lock, flags);
    return 0;
}

int ledger_persist(void) {
    uint64_t flags;
    spin_lock_irqsave(&g_ledger.lock, &flags);

    file_t *file = NULL;
    int res = vfs_open(g_ledger.storage_path, O_RDWR | O_CREAT, 0644, &file);
    if (res != 0) {
        spin_unlock_irqrestore(&g_ledger.lock, flags);
        return res;
    }

    ledger_file_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = LEDGER_MAGIC;
    hdr.version = LEDGER_VERSION;
    hdr.block_count = g_ledger.chain_height;
    hdr.total_tx_count = g_ledger.total_txs;
    if (g_ledger.chain_height > 0) {
        memcpy(hdr.latest_hash, g_ledger.chain[g_ledger.chain_height - 1].block_hash, SHA256_DIGEST_SIZE);
    }

    if (vfs_write(file, &hdr, sizeof(hdr)) != sizeof(hdr)) res = -STRATUM_EIO;
    for (uint64_t i = 0; res == 0 && i < g_ledger.chain_height; i++) {
        if (vfs_write(file, &g_ledger.chain[i], sizeof(ledger_block_t)) != sizeof(ledger_block_t)) {
            res = -STRATUM_EIO;
        }
    }

    vfs_close(file);
    if (res == 0) res = vfs_sync();

    spin_unlock_irqrestore(&g_ledger.lock, flags);
    return res;
}

int ledger_recover(void) {
    uint64_t flags;
    spin_lock_irqsave(&g_ledger.lock, &flags);

    file_t *file = NULL;
    int res = vfs_open(g_ledger.storage_path, O_RDONLY, 0644, &file);
    if (res != 0) {
        spin_unlock_irqrestore(&g_ledger.lock, flags);
        return res;
    }

    ledger_file_header_t hdr;
    int64_t rd = vfs_read(file, &hdr, sizeof(hdr));
    if (rd != sizeof(hdr) || hdr.magic != LEDGER_MAGIC || hdr.version != LEDGER_VERSION) {
        vfs_close(file);
        spin_unlock_irqrestore(&g_ledger.lock, flags);
        return -STRATUM_EINVAL;
    }

    uint64_t valid_blocks = 0;
    uint64_t valid_txs = 0;

    for (uint64_t i = 0; i < hdr.block_count && i < LEDGER_CHAIN_CAPACITY; i++) {
        ledger_block_t blk;
        rd = vfs_read(file, &blk, sizeof(blk));
        if (rd != sizeof(blk)) break;

        const ledger_block_t *prev = (valid_blocks > 0) ? &g_ledger.chain[valid_blocks - 1] : NULL;
        if (ledger_validate_block(&blk, prev) != 0) {
            break; /* Stop on corrupted or incomplete block */
        }

        g_ledger.chain[valid_blocks] = blk;
        valid_blocks++;
        valid_txs += blk.tx_count;
    }

    vfs_close(file);

    g_ledger.chain_height = valid_blocks;
    g_ledger.total_txs = valid_txs;
    g_ledger.mempool_count = 0;

    spin_unlock_irqrestore(&g_ledger.lock, flags);
    return (int)valid_blocks;
}

int ledger_get_latest_block(ledger_block_t *out_block) {
    if (!out_block) return -STRATUM_EINVAL;
    uint64_t flags;
    spin_lock_irqsave(&g_ledger.lock, &flags);
    if (g_ledger.chain_height == 0) {
        spin_unlock_irqrestore(&g_ledger.lock, flags);
        return -STRATUM_ENOENT;
    }
    *out_block = g_ledger.chain[g_ledger.chain_height - 1];
    spin_unlock_irqrestore(&g_ledger.lock, flags);
    return 0;
}

int ledger_replicate_to_peer(uint32_t peer_ip, uint16_t peer_port) {
    int sock = sys_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0) return sock;

    struct sockaddr_in saddr;
    memset(&saddr, 0, sizeof(saddr));
    saddr.sin_family = AF_INET;
    saddr.sin_port = htons(peer_port);
    saddr.sin_addr.s_addr = htonl(peer_ip);

    int conn = sys_connect(sock, (struct sockaddr *)&saddr, sizeof(saddr));
    if (conn != 0) {
        sys_shutdown(sock, 0);
        return conn;
    }

    /* Send Announcement Frame with latest block */
    ledger_block_t blk;
    int g_res = ledger_get_latest_block(&blk);
    if (g_res != 0) {
        sys_shutdown(sock, 0);
        return g_res;
    }

    ledger_net_hdr_t net_hdr;
    net_hdr.magic = LEDGER_NET_MAGIC;
    net_hdr.msg_type = LEDGER_MSG_ANNOUNCE_BLOCK;
    net_hdr.flags = 0;
    net_hdr.payload_len = sizeof(blk);
    net_hdr.checksum = 0;

    sys_send(sock, &net_hdr, sizeof(net_hdr), 0);
    sys_send(sock, &blk, sizeof(blk), 0);

    sys_shutdown(sock, 0);
    return 0;
}

int ledger_receive_replication_stream(int client_fd) {
    ledger_net_hdr_t hdr;
    int64_t r = sys_recv(client_fd, &hdr, sizeof(hdr), 0);
    if (r != sizeof(hdr) || hdr.magic != LEDGER_NET_MAGIC) {
        return -STRATUM_EINVAL;
    }

    if (hdr.msg_type == LEDGER_MSG_ANNOUNCE_BLOCK && hdr.payload_len == sizeof(ledger_block_t)) {
        ledger_block_t blk;
        r = sys_recv(client_fd, &blk, sizeof(blk), 0);
        if (r == sizeof(blk)) {
            return ledger_append_block(&blk);
        }
    }

    return 0;
}
