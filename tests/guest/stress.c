/* Short guest smoke workload for filesystem, pipes, hashing and local ledger. */

#include <kernel/kernel.h>
#include <kernel/types.h>
#include <kernel/vfs.h>
#include <kernel/pmm.h>
#include <kernel/sched.h>
#include <kernel/smp.h>
#include <kernel/ledger.h>
#include <shared/sha256.h>
#include <kernel/string.h>
#include <kernel/pipe.h>

static int soak_test_fs_churn(uint32_t cycle) {
    char path[64];
    snprintf(path, sizeof(path), "/strata/soak_%u.dat", cycle);

    file_t *f = NULL;
    int res = vfs_open(path, O_CREAT | O_WRONLY, 0644, &f);
    if (res != 0 || !f) {
        kprintf("[SOAK] ERROR: failed to open '%s' for write (res=%d)\n", path, res);
        return -1;
    }

    uint8_t payload[2048];
    for (size_t i = 0; i < sizeof(payload); i++) {
        payload[i] = (uint8_t)(0xAA ^ (i & 0xFF) ^ (cycle & 0xFF));
    }

    int64_t wr = vfs_write(f, payload, sizeof(payload));
    if (wr != sizeof(payload)) {
        kprintf("[SOAK] ERROR: write failed on '%s' (wr=%ld)\n", path, wr);
        vfs_close(f);
        return -2;
    }
    vfs_close(f);

    /* Read back and verify */
    f = NULL;
    res = vfs_open(path, O_RDONLY, 0, &f);
    if (res != 0 || !f) {
        kprintf("[SOAK] ERROR: failed to reopen '%s' for read\n", path);
        return -3;
    }

    uint8_t readback[2048];
    memset(readback, 0, sizeof(readback));
    int64_t rd = vfs_read(f, readback, sizeof(readback));
    vfs_close(f);

    if (rd != sizeof(payload)) {
        kprintf("[SOAK] ERROR: read failed on '%s' (rd=%ld)\n", path, rd);
        return -4;
    }

    if (memcmp(payload, readback, sizeof(payload)) != 0) {
        kprintf("[SOAK] ERROR: data mismatch on file '%s'\n", path);
        return -5;
    }

    /* Unlink file */
    return vfs_unlink(path);
}

static int soak_test_pipe_stream(void) {
    file_t *rf = NULL, *wf = NULL;
    int res = pipe_create(&rf, &wf);
    if (res != 0) {
        kprintf("[SOAK] ERROR: pipe_create failed (%d)\n", res);
        return -1;
    }

    const char msg[] = "SOAK_IPC_PIPE_PAYLOAD_TEST_DATA";
    int64_t wr = wf->ops->write(wf, msg, sizeof(msg));
    if (wr != sizeof(msg)) {
        file_close(rf);
        file_close(wf);
        return -2;
    }

    char recv_buf[64];
    memset(recv_buf, 0, sizeof(recv_buf));
    int64_t rd = rf->ops->read(rf, recv_buf, sizeof(recv_buf));
    file_close(rf);
    file_close(wf);

    if (rd != sizeof(msg) || strcmp(msg, recv_buf) != 0) {
        kprintf("[SOAK] ERROR: pipe read mismatch\n");
        return -3;
    }
    return 0;
}

static int soak_test_crypto_hash(uint32_t cycle) {
    uint8_t data[256];
    memset(data, (int)(cycle & 0xFF), sizeof(data));

    uint8_t hash[32];
    sha256_hash(data, sizeof(data), hash);

    /* Verify non-zero hash */
    uint32_t non_zero = 0;
    for (int i = 0; i < 32; i++) {
        if (hash[i] != 0) non_zero++;
    }
    return (non_zero > 0) ? 0 : -1;
}

int soak_stress_run(uint32_t cycles) {
    kprintf("[SOAK] Starting bounded guest stress workload.\n");
    kprintf("[SOAK] Target configuration: %u vCPUs online, active I/O, IPC, and ledger.\n",
            smp_get_online_cpus());

    uint64_t initial_free_pages = pmm_get_free_pages();

    for (uint32_t c = 1; c <= cycles; c++) {
        /* 1. Filesystem churn */
        int fs_res = soak_test_fs_churn(c);
        kassert(fs_res == 0);

        /* 2. IPC pipe stream */
        int pipe_res = soak_test_pipe_stream();
        kassert(pipe_res == 0);

        /* 3. Cryptographic hashing */
        int hash_res = soak_test_crypto_hash(c);
        kassert(hash_res == 0);

        /* 4. Distributed durable ledger transactions */
        kassert(ledger_submit_tx(8000 + c, 9000 + c, 50 * c, "soak_tx", 7) == 0);
        ledger_block_t blk;
        int b_res = ledger_create_block(timer_get_uptime_ms(), &blk);
        kassert(b_res == 0);
        kassert(ledger_append_block(&blk) == 0);
        kassert(ledger_persist() == 0);

        /* 5. Memory footprint verification */
        uint64_t cur_free_pages = pmm_get_free_pages();
        uint64_t free_kib = (cur_free_pages * 4096) / 1024;

        kprintf("[SOAK] Cycle %u/%u completed: CPUs=%u, Uptime=%lu ms, FreeMem=%lu KiB, Status=OK\n",
                c, cycles, smp_get_online_cpus(), timer_get_uptime_ms(), free_kib);
    }

    uint64_t final_free_pages = pmm_get_free_pages();
    /* Verify memory remained flat (within 16 pages variance) */
    int64_t diff = (int64_t)initial_free_pages - (int64_t)final_free_pages;
    if (diff < 0) diff = -diff;
    kassert(diff <= 16);

    kprintf("[SOAK] Completed %u cycles. Free-page difference: %ld pages.\n",
            cycles, diff);
    return 0;
}
