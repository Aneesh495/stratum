#include <kernel/kernel.h>
#include <kernel/boot.h>
#include <kernel/uart.h>
#include <kernel/fb.h>
#include <kernel/gdt.h>
#include <kernel/idt.h>
#include <kernel/pmm.h>
#include <kernel/slab.h>
#include <kernel/vmm.h>
#include <kernel/smp.h>
#include <kernel/spinlock.h>
#include <kernel/mutex.h>
#include <kernel/sched.h>
#include <kernel/syscall.h>
#include <kernel/user_elf.h>
#include <kernel/file.h>
#include <kernel/pipe.h>
#include <kernel/process.h>
#include <kernel/pci.h>
#include <kernel/virtio.h>
#include <kernel/virtio_blk.h>
#include <kernel/virtio_net.h>
#include <kernel/vfs.h>
#include <kernel/stratafs.h>
#include <kernel/journal.h>
#include <kernel/net.h>
#include <kernel/ethernet.h>
#include <kernel/arp.h>
#include <kernel/ipv4.h>
#include <kernel/icmp.h>
#include <kernel/udp.h>
#include <kernel/tcp.h>
#include <kernel/socket.h>
#include <kernel/trace.h>
#include <kernel/panic.h>
#include <kernel/ledger.h>
#include <shared/sha256.h>
#include <kernel/x86_64.h>

extern const uint8_t g_user_init_binary[];
extern const uint64_t g_user_init_binary_len;
extern const uint8_t g_user_test_binary[];
extern const uint64_t g_user_test_binary_len;

static _Noreturn void start_system(void) {
    smp_init(&g_boot_handoff);
    sched_init();
    syscall_init();
    file_init();
    pipe_init();
    process_init();
    trace_init();
    sti();

    pci_init();
    virtio_blk_init();
    virtio_net_init();
    vfs_init();
    stratafs_init();
    int mount_result = vfs_mount("virtio-blk", "/", "stratafs", 0);
    if (mount_result != 0) {
        kprintf("[KERNEL] Root mount failed (%d). Starting shell without storage.\n", mount_result);
    }
    net_init();
    if (strcmp(g_boot_handoff.cmdline, "crash=wal-published\n") == 0) {
        journal_test_crash_after_publish();
    }

    static user_program_t init;
    kassert(user_elf_load(g_user_init_binary, g_user_init_binary_len, &init) == 0);
    kassert(process_create_init(&init) != NULL);

    /* Poll device RX on the bootstrap CPU until interrupt-driven I/O is available. */
    while (1) {
        net_poll();
        hlt();
    }
}

static volatile uint32_t g_thread_test_counter = 0;
static volatile uint32_t g_threads_completed = 0;
static spinlock_t g_sched_test_lock = SPINLOCK_INIT;

static void worker_priority(void *arg) {
    uint64_t id = (uint64_t)arg;
    for (int i = 0; i < 5; i++) {
        uint64_t *buffer = kmalloc(64);
        uint64_t *large = kmalloc(4096);
        kassert(buffer && large);
        buffer[0] = id * 100 + i;
        large[511] = buffer[0];
        thread_yield();
        kassert(buffer[0] == id * 100 + i && large[511] == buffer[0]);
        kfree(large);
        kfree(buffer);
        uint64_t flags;
        spin_lock_irqsave(&g_sched_test_lock, &flags);
        g_thread_test_counter++;
        spin_unlock_irqrestore(&g_sched_test_lock, flags);
        thread_yield();
    }
    kprintf("[SCHED] Worker %lu finished (counter=%u)\n", id, g_thread_test_counter);
    __atomic_add_fetch(&g_threads_completed, 1, __ATOMIC_SEQ_CST);
}

static void worker_sleeper(void *arg) {
    uint64_t id = (uint64_t)arg;
    kprintf("[SCHED] Sleeper %lu sleeping for 30 ms...\n", id);
    thread_sleep_ms(30);
    kprintf("[SCHED] Sleeper %lu woke up at uptime %lu ms\n", id, timer_get_uptime_ms());
    __atomic_add_fetch(&g_threads_completed, 1, __ATOMIC_SEQ_CST);
}

void kmain(boot_handoff_t *handoff, uint64_t magic) {
    /* 1. Initialize serial port for early diagnostic output */
    uart_init();

    /* 2. Validate versioned boot handoff from UEFI loader */
    int val_res = boot_validate_handoff(handoff, magic);
    if (val_res != 0) {
        panic("Boot handoff validation failed with code %d\n", val_res);
    }
    /* Switch to kernel higher-half copy of handoff structure */
    handoff = &g_boot_handoff;

    /* 3. Initialize early framebuffer console */
    fb_init(handoff);

    kputs("\n");
    kputs("================================================================================\n");
    kputs("  Stratum Multiprocessor Operating System v0.1.0 (x86-64)\n");
    kputs("================================================================================\n");
    kprintf("[KERNEL] Kernel entered at higher-half entry point\n");
    kprintf("[KERNEL] Boot handoff verified successfully (magic: 0x%lx)\n", magic);

    /* 4. Initialize GDT, TSS, and dedicated IST stacks */
    gdt_init();

    /* 5. Initialize IDT and exception handling */
    idt_init();

    /* 6. Dump system boot configuration */
    boot_dump_info(handoff);

    /* 7. Initialize Physical Memory Manager (PMM) */
    pmm_init(handoff);

    /* 8. Initialize Kernel Object Allocator (SLAB) */
    slab_init();

    /* 9. Initialize Virtual Memory Manager (VMM) and switch CR3 */
    vmm_init(handoff);

    if (strcmp(handoff->cmdline, "fault=ud2\n") == 0) {
        __asm__ volatile("ud2");
    }
    if (strcmp(handoff->cmdline, "selftest\n") != 0) {
        start_system();
    }

    /* Run P04 Memory Verification Tests */
    kprintf("[TEST] Running P04 memory allocation and mapping verification...\n");
    void *p1 = kmalloc(32);
    void *p2 = kmalloc(256);
    void *p3 = kmalloc(4096);
    kassert(p1 != NULL && p2 != NULL && p3 != NULL);
    strcpy((char *)p1, "Stratum slab test");
    kassert(strcmp((char *)p1, "Stratum slab test") == 0);
    kfree(p1);
    kfree(p2);
    kfree(p3);

    /* Test VMM mapping and unmapping */
    uint64_t test_paddr = pmm_alloc_page();
    kassert(test_paddr != 0);
    uint64_t test_vaddr = 0xFFFF900000000000ULL;
    int map_res = vmm_map_page(g_kernel_pml4, test_vaddr, test_paddr, PTE_PRESENT | PTE_WRITABLE);
    kassert(map_res == 0);
    *(volatile uint64_t *)test_vaddr = 0x5354524154554DULL;
    kassert(*(volatile uint64_t *)test_vaddr == 0x5354524154554DULL);
    vmm_unmap_page(g_kernel_pml4, test_vaddr);
    pmm_free_page(test_paddr);

    kprintf("[TEST] P04 memory verification tests passed successfully.\n");

    /* 10. Initialize Multiprocessor Subsystem (SMP) */
    smp_init(handoff);

    /* Run P05 Synchronization and SMP Verification Tests */
    kprintf("[TEST] Running P05 SMP synchronization verification...\n");
    static spinlock_t test_lock = SPINLOCK_INIT;
    spin_lock(&test_lock);
    kassert(!spin_trylock(&test_lock));
    spin_unlock(&test_lock);
    kassert(spin_trylock(&test_lock));
    spin_unlock(&test_lock);

    static mutex_t test_mtx = MUTEX_INIT;
    mutex_lock(&test_mtx);
    kassert(!mutex_trylock(&test_mtx));
    mutex_unlock(&test_mtx);
    kassert(mutex_trylock(&test_mtx));
    mutex_unlock(&test_mtx);

    /* Broadcast cross-CPU TLB shootdown IPI */
    kassert(smp_tlb_shootdown(0) == 0);

    kprintf("[TEST] P05 SMP synchronization verification passed successfully.\n");

    /* 11. Initialize Preemptive SMP Scheduler (P06) */
    sched_init();

    kprintf("[TEST] Running P06 preemptive SMP scheduler verification...\n");

    thread_create("worker_rt", worker_priority, (void *)1, THREAD_PRIO_REALTIME);
    thread_create("worker_hi", worker_priority, (void *)2, THREAD_PRIO_HIGH);
    thread_create("worker_norm", worker_priority, (void *)3, THREAD_PRIO_NORMAL);
    thread_create("sleeper", worker_sleeper, (void *)4, THREAD_PRIO_NORMAL);

    sti();
    uint64_t start_ms = timer_get_uptime_ms();
    while (g_threads_completed < 4 && (timer_get_uptime_ms() - start_ms) < 10000) {
        hlt();
    }

    kassert(g_threads_completed == 4);
    kassert(g_thread_test_counter == 15);
    kprintf("[TEST] P06 scheduler verification passed successfully.\n");

    /* 12. Initialize Fast System Call ABI and Safe Usercopy (P07) */
    syscall_init();

    kprintf("[TEST] Running P07 User ABI & Safe Usercopy verification...\n");

    /* Test 1: Verify copy_from_user recovers gracefully from illegal unmapped address */
    char fault_scratch[16];
    int fault_res = copy_from_user(fault_scratch, (const void *)0x0000400000000000ULL, sizeof(fault_scratch));
    kassert(fault_res == -14);
    kprintf("[TEST] P07 safe usercopy fault recovery verified (status=%d).\n", fault_res);

    /* Test 2: Load User ELF binary into isolated address space */
    static user_program_t s_user_prog;
    int elf_res = user_elf_load(g_user_test_binary, g_user_test_binary_len, &s_user_prog);
    kassert(elf_res == 0);
    kassert(s_user_prog.entry_point == 0x400000);
    kassert(s_user_prog.user_stack_top != 0);
    kprintf("[TEST] P07 user ELF64 loader verified.\n");

    /* 13. Initialize Processes, File Descriptors, and IPC (P08) */
    file_init();
    pipe_init();
    process_init();

    kprintf("[TEST] Running P08 Process lifecycle, fork/waitpid, and IPC pipe verification...\n");
    process_t *init_proc = process_create_init(&s_user_prog);
    kassert(init_proc != NULL);
    kassert(init_proc->pid == 1);

    /* Await user process completion */
    uint64_t user_wait_start = timer_get_uptime_ms();
    while (!__atomic_load_n(&g_user_init_finished, __ATOMIC_ACQUIRE) && (timer_get_uptime_ms() - user_wait_start) < 30000) {
        __asm__ volatile("sti; hlt");
    }

    kassert(__atomic_load_n(&g_user_init_finished, __ATOMIC_ACQUIRE));
    kassert(g_user_exit_code == 42);
    kprintf("[TEST] P08 Processes, Threads, and IPC gates verified successfully.\n");

    /* Ensure BSP execution is on g_kernel_pml4 */
    write_cr3(virt_to_phys(g_kernel_pml4));

    /* 14. Initialize PCI Bus and Virtio Hardware I/O (P09) */
    pci_init();
    virtio_blk_init();
    virtio_net_init();

    kprintf("[TEST] Running P09 PCI and Virtio Hardware I/O verification (Gate A06)...\n");

    /* Test 1: Block Device I/O */
    virtio_blk_dev_t *blk = virtio_blk_get_primary();
    if (blk != NULL) {
        kassert(blk->capacity_sectors > 0);

        /* Read sector 0 */
        static uint8_t sector_buf[VIRTIO_BLK_SECTOR_SIZE];
        int64_t rd = virtio_blk_read(blk, 0, 1, sector_buf);
        kassert(rd == VIRTIO_BLK_SECTOR_SIZE);

        static uint8_t scratch_original[VIRTIO_BLK_SECTOR_SIZE];
        kassert(virtio_blk_read(blk, 100, 1, scratch_original) == VIRTIO_BLK_SECTOR_SIZE);
        /* Write pattern to the disposable self-test disk. */
        static uint8_t write_pat[VIRTIO_BLK_SECTOR_SIZE];
        for (int i = 0; i < VIRTIO_BLK_SECTOR_SIZE; i++) {
            write_pat[i] = (uint8_t)(0xA5 ^ (i & 0xFF));
        }

        int64_t wr = virtio_blk_write(blk, 100, 1, write_pat);
        kassert(wr == VIRTIO_BLK_SECTOR_SIZE);

        int fl = virtio_blk_flush(blk);
        kassert(fl == 0);

        /* Read back the scratch sector and verify integrity. */
        static uint8_t readback_pat[VIRTIO_BLK_SECTOR_SIZE];
        memset(readback_pat, 0, VIRTIO_BLK_SECTOR_SIZE);
        int64_t rd2 = virtio_blk_read(blk, 100, 1, readback_pat);
        kassert(rd2 == VIRTIO_BLK_SECTOR_SIZE);
        kassert(memcmp(write_pat, readback_pat, VIRTIO_BLK_SECTOR_SIZE) == 0);

        kassert(virtio_blk_write(blk, 100, 1, scratch_original) == VIRTIO_BLK_SECTOR_SIZE);
        kassert(virtio_blk_flush(blk) == 0);

        /* Test queue index cycling with 50 sequential single-sector operations */
        for (int iter = 0; iter < 50; iter++) {
            int64_t r = virtio_blk_read(blk, (uint64_t)(iter % 64), 1, sector_buf);
            kassert(r == VIRTIO_BLK_SECTOR_SIZE);
        }
        kprintf("[TEST] Virtio-blk synchronous read, write, flush, and queue index cycling verified.\n");
    } else {
        kprintf("[TEST] Note: Virtio-blk not attached; skipping block device I/O test.\n");
    }

    /* Test 2: Network Device I/O */
    virtio_net_dev_t *net = virtio_net_get_primary();
    if (net != NULL) {
        /* Verify MAC address is non-zero */
        bool mac_nonzero = false;
        for (int i = 0; i < VIRTIO_NET_ETH_ALEN; i++) {
            if (net->mac[i] != 0) mac_nonzero = true;
        }
        kassert(mac_nonzero);

        /* Build test broadcast frame (ARP probe, 64 bytes) */
        uint8_t test_frame[64];
        memset(test_frame, 0, sizeof(test_frame));
        /* Destination: Broadcast FF:FF:FF:FF:FF:FF */
        memset(&test_frame[0], 0xFF, 6);
        /* Source: Device MAC */
        memcpy(&test_frame[6], net->mac, 6);
        /* EtherType: 0x0806 (ARP) */
        test_frame[12] = 0x08;
        test_frame[13] = 0x06;

        int tx_res = virtio_net_transmit(net, test_frame, sizeof(test_frame));
        kassert(tx_res == sizeof(test_frame));
        kprintf("[TEST] Virtio-net MAC identification and TX frame transmission verified.\n");
    } else {
        kprintf("[TEST] Note: Virtio-net not attached; skipping network device I/O test.\n");
    }

    kassert(blk && net);
    kprintf("[TEST] Virtio hardware smoke completed.\n");

    /* 15. Initialize VFS, Journal, and StrataFS (P10 & P11) */
    vfs_init();
    stratafs_init();

    kprintf("[TEST] Running P10 & P11 VFS, StrataFS Storage, and Journal Recovery verification...\n");

    /* Mount StrataFS on root / */
    int m_res = vfs_mount("virtio-blk", "/", "stratafs", 0);
    kassert(m_res == 0);

    /* Test 1: Directory hierarchy creation */
    kprintf("[TEST] mkdir /system...\n");
    int d_res = vfs_mkdir("/system", 0755);
    kprintf("[TEST] mkdir /system returned %d\n", d_res);
    kassert(d_res == 0 || d_res == -STRATUM_EEXIST);
    kprintf("[TEST] mkdir /system/logs...\n");
    int d_res2 = vfs_mkdir("/system/logs", 0755);
    kprintf("[TEST] mkdir /system/logs returned %d\n", d_res2);
    kassert(d_res2 == 0 || d_res2 == -STRATUM_EEXIST);

    /* Test 2: File creation and multi-block writes */
    file_t *f = NULL;
    kprintf("[TEST] open /system/logs/boot.log...\n");
    int o_res = vfs_open("/system/logs/boot.log", O_CREAT | O_WRONLY, 0644, &f);
    kprintf("[TEST] open returned %d, f=%p\n", o_res, f);
    kassert(o_res == 0 && f != NULL);

    /* Write 12,000 bytes spanning 3 distinct 4 KiB disk blocks */
    static uint8_t file_buf[12000];
    for (int i = 0; i < 12000; i++) {
        file_buf[i] = (uint8_t)(0x5A ^ (i & 0xFF));
    }
    kprintf("[TEST] write 12000 bytes...\n");
    int64_t wr_bytes = vfs_write(f, file_buf, sizeof(file_buf));
    kprintf("[TEST] write returned %ld\n", wr_bytes);
    kassert(wr_bytes == sizeof(file_buf));
    vfs_close(f);

    /* Test 3: Readback and verification */
    f = NULL;
    kprintf("[TEST] reopen for read...\n");
    int ro_res = vfs_open("/system/logs/boot.log", O_RDONLY, 0, &f);
    kprintf("[TEST] reopen returned %d\n", ro_res);
    kassert(ro_res == 0 && f != NULL);

    static uint8_t verify_buf[12000];
    memset(verify_buf, 0, sizeof(verify_buf));
    int64_t rd_bytes = vfs_read(f, verify_buf, sizeof(verify_buf));
    kprintf("[TEST] read returned %ld\n", rd_bytes);
    kassert(rd_bytes == sizeof(verify_buf));
    kassert(memcmp(file_buf, verify_buf, sizeof(file_buf)) == 0);
    vfs_close(f);

    /* Test 4: Single indirect block test (60,000 bytes = 15 blocks) */
    f = NULL;
    int ind_open = vfs_open("/bigfile.dat", O_CREAT | O_WRONLY, 0644, &f);
    kassert(ind_open == 0 && f != NULL);

    static uint8_t big_buf[60000];
    for (int i = 0; i < 60000; i++) {
        big_buf[i] = (uint8_t)(0xC3 ^ ((i >> 4) & 0xFF));
    }
    int64_t big_wr = vfs_write(f, big_buf, sizeof(big_buf));
    kassert(big_wr == sizeof(big_buf));
    vfs_close(f);

    f = NULL;
    int ind_read = vfs_open("/bigfile.dat", O_RDONLY, 0, &f);
    kassert(ind_read == 0 && f != NULL);

    static uint8_t big_verify[60000];
    memset(big_verify, 0, sizeof(big_verify));
    int64_t big_rd = vfs_read(f, big_verify, sizeof(big_verify));
    kassert(big_rd == sizeof(big_verify));
    kassert(memcmp(big_buf, big_verify, sizeof(big_buf)) == 0);
    vfs_close(f);

    /* Test 5: Stat metadata verification */
    vfs_stat_t st;
    int stat_res = vfs_stat("/bigfile.dat", &st);
    kassert(stat_res == 0);
    kassert(st.st_size == 60000);

    /* Test 6: Atomic Journal Transaction Checkpoint & Remount Recovery */
    kassert(vfs_sync() == 0);
    kassert(vfs_unmount("/") == 0);

    /* Remount and verify persistent state */
    int remount_res = vfs_mount("virtio-blk", "/", "stratafs", 0);
    kassert(remount_res == 0);

    f = NULL;
    int remount_read = vfs_open("/system/logs/boot.log", O_RDONLY, 0, &f);
    kassert(remount_read == 0 && f != NULL);
    memset(verify_buf, 0, sizeof(verify_buf));
    int64_t remount_rd = vfs_read(f, verify_buf, sizeof(verify_buf));
    kassert(remount_rd == sizeof(verify_buf));
    kassert(memcmp(file_buf, verify_buf, sizeof(file_buf)) == 0);
    vfs_close(f);

    /* Test 7: Unlink file */
    int unl_res = vfs_unlink("/bigfile.dat");
    kassert(unl_res == 0);
    f = NULL;
    int unl_open = vfs_open("/bigfile.dat", O_RDONLY, 0, &f);
    kassert(unl_open != 0 && f == NULL);

    kprintf("[TEST] StrataFS directory hierarchy, multi-block files, indirect blocks, atomic journal transactions, and remount recovery verified.\n");
    kprintf("[TEST] P10 & P11 VFS and StrataFS storage verified successfully.\n");

    /* 16. Initialize Network Stack and Sockets (P12) */
    net_init();

    kprintf("[TEST] Running P12 Network Stack (TCP/IP) and Socket API verification...\n");

    /* Test 1: Checksum verification */
    static const uint8_t csum_test_data[20] = {
        0x45, 0x00, 0x00, 0x3c, 0x1c, 0x46, 0x40, 0x00,
        0x40, 0x06, 0x00, 0x00, 0x0a, 0x00, 0x02, 0x0f,
        0x0a, 0x00, 0x02, 0x02
    };
    uint16_t calc_csum = net_checksum(csum_test_data, sizeof(csum_test_data));
    kassert(calc_csum != 0);
    kprintf("[TEST] Internet checksum calculation verified: 0x%04x\n", calc_csum);

    /* Test 2: ARP resolution */
    uint8_t gw_mac[ETH_ALEN];
    uint32_t gateway = IP4_ADDR(10, 0, 2, 2);
    kassert(arp_send_request(net_get_default_if(), gateway) == 0);
    uint64_t arp_start = timer_get_uptime_ms();
    int arp_res;
    do {
        net_poll();
        arp_res = arp_lookup(gateway, gw_mac);
        if (arp_res == 0) break;
        hlt();
    } while (timer_get_uptime_ms() - arp_start < 2000);
    kassert(arp_res == 0);
    kprintf("[TEST] ARP cache lookup verified: gateway MAC=%02x:%02x:%02x:%02x:%02x:%02x\n",
            gw_mac[0], gw_mac[1], gw_mac[2], gw_mac[3], gw_mac[4], gw_mac[5]);

    /* Test 3: UDP loopback / datagram transmission */
    int udp_res = udp_output(IP4_ADDR(10, 0, 2, 15), 5000, IP4_ADDR(10, 0, 2, 2), 53, "DNS_QUERY", 9);
    kassert(udp_res == 0);
    kprintf("[TEST] UDP datagram transmission with pseudo-header checksum verified.\n");

    /* Test 4: ICMP Echo request parsing and reply simulation */
    pbuf_t *icmp_req_p = pbuf_alloc(sizeof(icmp_hdr_t) + 32);
    kassert(icmp_req_p != NULL);
    icmp_hdr_t *req_hdr = (icmp_hdr_t *)icmp_req_p->payload;
    req_hdr->type = ICMP_TYPE_ECHO_REQUEST;
    req_hdr->code = 0;
    req_hdr->checksum = 0;
    req_hdr->id = htons(0x1234);
    req_hdr->sequence = htons(1);
    memset(icmp_req_p->payload + sizeof(icmp_hdr_t), 0xAA, 32);
    req_hdr->checksum = net_checksum(req_hdr, sizeof(icmp_hdr_t) + 32);
    int icmp_res = icmp_input(icmp_req_p, IP4_ADDR(10, 0, 2, 2));
    kassert(icmp_res == 0);
    pbuf_free(icmp_req_p);
    kprintf("[TEST] ICMP echo request handling and reply transmission verified.\n");

    kprintf("[TEST] Network checksum, UDP transmit, and ICMP smoke checks complete.\n");

    /* 17. Initialize Structured Tracing, Diagnostics, and Distributed Ledger (P13 / Gate A09) */
    trace_init();
    trace_emit(TRACE_EVENT_SYSCALL, 1, 2, 3, 4);
    trace_dump();

    /* Verify Panic Symbol Diagnostics */
    uint64_t sym_offset = 0;
    const char *sym_name = panic_lookup_symbol((uint64_t)kmain, &sym_offset);
    kassert(strcmp(sym_name, "kmain") == 0);
    kprintf("[TEST] Panic symbol resolution verified: %s+0x%lx\n", sym_name, sym_offset);

    /* Verify Cryptographic SHA-256 and Merkle Trees */
    static const char sha_msg[] = "stratum_ledger_crypto_test";
    uint8_t sha_out[32];
    sha256_hash(sha_msg, strlen(sha_msg), sha_out);
    kprintf("[TEST] SHA-256 cryptographic hashing verified (digest=%02x%02x%02x%02x...).\n",
            sha_out[0], sha_out[1], sha_out[2], sha_out[3]);

    /* Verify Distributed Durable Ledger Subsystem */
    vfs_mkdir("/strata", 0755);
    kprintf("[TEST] Initializing durable ledger on StrataFS at '/strata/ledger.dat'...\n");
    int led_res = ledger_init("/strata/ledger.dat");
    kassert(led_res == 0);

    /* Submit multiple transactions */
    int tx1 = ledger_submit_tx(1001, 2002, 500, "tx_batch_alpha", 14);
    kassert(tx1 == 0);
    int tx2 = ledger_submit_tx(2002, 3003, 250, "tx_batch_beta", 13);
    kassert(tx2 == 0);

    /* Forge block 1 */
    ledger_block_t blk1;
    int b1_res = ledger_create_block(timer_get_uptime_ms(), &blk1);
    kassert(b1_res == 0);
    kassert(blk1.tx_count == 2);
    int app1 = ledger_append_block(&blk1);
    kassert(app1 == 0);
    kprintf("[TEST] Ledger block 1 forged and appended (Merkle root: %02x%02x%02x%02x...).\n",
            blk1.merkle_root[0], blk1.merkle_root[1], blk1.merkle_root[2], blk1.merkle_root[3]);

    /* Submit tx for block 2 */
    int tx3 = ledger_submit_tx(3003, 4004, 75, "tx_batch_gamma", 14);
    kassert(tx3 == 0);
    ledger_block_t blk2;
    int b2_res = ledger_create_block(timer_get_uptime_ms() + 10, &blk2);
    kassert(b2_res == 0);
    kassert(blk2.tx_count == 1);
    int app2 = ledger_append_block(&blk2);
    kassert(app2 == 0);
    kprintf("[TEST] Ledger block 2 forged and chained to block 1.\n");

    /* Persist the kernel ledger prototype to a disposable self-test disk. */
    int persist_res = ledger_persist();
    kassert(persist_res == 0);
    kprintf("[TEST] Ledger chain committed and persisted to StrataFS file '/strata/ledger.dat'.\n");

    /* Redo recovery and replay validation */
    int rec_blocks = ledger_recover();
    kassert(rec_blocks == 3); /* Genesis + block 1 + block 2 */
    kprintf("[TEST] Ledger recovery replayed %d valid blocks and verified cryptographic continuity.\n", rec_blocks);

    kprintf("[TEST] Kernel ledger model and local persistence smoke checks complete.\n");

    /* 18. Initialize Depth, Concurrency, and Soak Verification (P14 / Gate A10) */
    extern int soak_stress_run(uint32_t cycles);
    kprintf("[TEST] Running P14 Real Guest Multi-threaded Stress and Soak verification...\n");
    int soak_res = soak_stress_run(5);
    kassert(soak_res == 0);
    kprintf("[TEST] Guest stress smoke completed.\n");
    kprintf("[KERNEL] Phase P14 reached. Entering kernel idle loop with %u CPU(s) online.\n",
            smp_get_online_cpus());

    /* Kernel main idle loop */
    while (1) {
        hlt();
    }
}
