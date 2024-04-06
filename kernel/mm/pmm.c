#include <kernel/pmm.h>
#include <kernel/spinlock.h>
#include <kernel/kernel.h>
#include <kernel/string.h>

#define MAX_SUPPORTED_PAGES (1024 * 1024) /* 4 GiB of RAM (1M pages) */
#define BITMAP_WORDS        (MAX_SUPPORTED_PAGES / 64)

uint64_t g_kernel_phys_base = 0;

static uint64_t g_page_bitmap[BITMAP_WORDS];
static spinlock_t pmm_lock = SPINLOCK_INIT;
static page_t  *g_pages = NULL;
static uint64_t g_num_pages = 0;

static uint64_t g_max_phys_addr = 0;
static uint64_t g_total_pages = 0;
static uint64_t g_free_pages = 0;
static uint64_t g_used_pages = 0;

static inline void bitmap_set(uint64_t pfn) {
    g_page_bitmap[pfn / 64] |= (1ULL << (pfn % 64));
}

static inline void bitmap_clear(uint64_t pfn) {
    g_page_bitmap[pfn / 64] &= ~(1ULL << (pfn % 64));
}

static inline bool bitmap_test(uint64_t pfn) {
    return (g_page_bitmap[pfn / 64] & (1ULL << (pfn % 64))) != 0;
}

static void mark_range_reserved(uint64_t start_phys, uint64_t size_bytes) {
    uint64_t start_pfn = start_phys / PAGE_SIZE;
    uint64_t end_pfn = (start_phys + size_bytes + PAGE_SIZE - 1) / PAGE_SIZE;

    for (uint64_t pfn = start_pfn; pfn < end_pfn && pfn < g_num_pages; pfn++) {
        if (!bitmap_test(pfn)) {
            bitmap_set(pfn);
            if (g_pages) {
                g_pages[pfn].flags = PAGE_FLAG_RESERVED;
                g_pages[pfn].refcount = 1;
            }
            if (g_free_pages > 0) g_free_pages--;
            g_used_pages++;
        }
    }
}

void pmm_init(const boot_handoff_t *handoff) {
    memset(g_page_bitmap, 0xFF, sizeof(g_page_bitmap));

    g_kernel_phys_base = handoff->kernel_phys_base;

    g_total_pages = 0;
    g_free_pages = 0;
    g_used_pages = 0;
    g_max_phys_addr = 0;

    boot_mem_desc_t *descs = (boot_mem_desc_t *)phys_to_virt(handoff->mem_map_phys);

    /* 1. First pass: Discover highest physical address of system RAM */
    for (uint32_t i = 0; i < handoff->mem_map_entries; i++) {
        if (descs[i].type != BOOT_MEM_RESERVED && descs[i].type != BOOT_MEM_FRAMEBUFFER) {
            uint64_t end_addr = descs[i].phys_addr + descs[i].page_count * PAGE_SIZE;
            if (end_addr > g_max_phys_addr) {
                g_max_phys_addr = end_addr;
            }
        }
    }

    g_num_pages = g_max_phys_addr / PAGE_SIZE;
    if (g_num_pages > MAX_SUPPORTED_PAGES) {
        g_num_pages = MAX_SUPPORTED_PAGES;
    }

    /* 2. Locate memory for page_t structures array */
    uint64_t array_bytes = g_num_pages * sizeof(page_t);
    uint64_t array_phys = 0;
    for (uint32_t i = 0; i < handoff->mem_map_entries; i++) {
        if (descs[i].type == BOOT_MEM_USABLE) {
            if (descs[i].phys_addr >= 0x100000 && descs[i].phys_addr < STRATUM_BOOT_PHYS_LIMIT &&
                array_bytes <= STRATUM_BOOT_PHYS_LIMIT - descs[i].phys_addr &&
                descs[i].page_count * PAGE_SIZE >= array_bytes) {
                array_phys = descs[i].phys_addr;
                break;
            }
        }
    }

    if (array_phys == 0) {
        panic("Cannot reserve physical memory for page allocator metadata.");
    }

    g_pages = (page_t *)phys_to_virt(array_phys);
    for (uint64_t i = 0; i < g_num_pages; i++) {
        g_pages[i].flags = PAGE_FLAG_RESERVED;
        g_pages[i].refcount = 1;
        g_pages[i].order = 0;
        g_pages[i].next = NULL;
    }
    /* 3. Third pass: Populate usable page frames */
    for (uint32_t i = 0; i < handoff->mem_map_entries; i++) {
        uint64_t end_addr = descs[i].phys_addr + descs[i].page_count * PAGE_SIZE;

        if (descs[i].type == BOOT_MEM_USABLE) {
            uint64_t start_pfn = descs[i].phys_addr / PAGE_SIZE;
            uint64_t end_pfn = end_addr / PAGE_SIZE;

            for (uint64_t pfn = start_pfn; pfn < end_pfn && pfn < g_num_pages; pfn++) {
                bitmap_clear(pfn);
                g_pages[pfn].flags = PAGE_FLAG_FREE;
                g_pages[pfn].refcount = 0;
                g_total_pages++;
                g_free_pages++;
            }
        }
    }

    /* 4. Fourth pass: Mark critical kernel and boot regions as reserved */
    /* Reserve first 1 MiB (BIOS/IVT/EBDA/VGA legacy memory) */
    mark_range_reserved(0, 0x100000);

    /* Reserve g_pages structure array memory */
    mark_range_reserved(array_phys, array_bytes);

    /* Reserve kernel image physical pages */
    mark_range_reserved(handoff->kernel_phys_base, handoff->kernel_phys_size);

    /* Reserve initramfs if present */
    if (handoff->initramfs_size > 0) {
        mark_range_reserved(handoff->initramfs_phys, handoff->initramfs_size);
    }

    /* Reserve handoff structure itself and memory map */
    mark_range_reserved(handoff->mem_map_phys, handoff->mem_map_entries * handoff->mem_map_entry_size);

    kprintf("[PMM] Physical memory manager initialized:\n");
    kprintf("[PMM]   Total RAM: %lu MiB (%lu pages), Usable: %lu MiB (%lu free pages)\n",
            g_max_phys_addr / (1024 * 1024), g_max_phys_addr / PAGE_SIZE,
            g_free_pages * PAGE_SIZE / (1024 * 1024), g_free_pages);
}

uint64_t pmm_alloc_pages(uint32_t order) {
    if (order >= 21) return 0;
    uint64_t count = 1ULL << order;
    uint64_t flags;
    spin_lock_irqsave(&pmm_lock, &flags);
    uint64_t result = 0;
    for (uint64_t pfn = 0; pfn + count <= g_num_pages; pfn += count) {
        uint64_t i = 0;
        while (i < count && !bitmap_test(pfn + i)) i++;
        if (i != count) continue;
        for (i = 0; i < count; i++) {
            bitmap_set(pfn + i);
            g_pages[pfn + i].flags = PAGE_FLAG_USER;
            g_pages[pfn + i].refcount = 1;
            g_pages[pfn + i].order = order;
        }
        g_free_pages -= count;
        g_used_pages += count;
        result = pfn * PAGE_SIZE;
        break;
    }
    spin_unlock_irqrestore(&pmm_lock, flags);

    /* Ownership is exclusive after marking the pages allocated. */
    if (result) memset(phys_to_virt(result), 0, count * PAGE_SIZE);
    return result;
}

uint64_t pmm_alloc_page(void) {
    return pmm_alloc_pages(0);
}

static bool allocated_page(uint64_t pfn) {
    return pfn < g_num_pages && g_pages[pfn].flags == PAGE_FLAG_USER &&
        g_pages[pfn].refcount > 0;
}

void pmm_free_pages(uint64_t paddr, uint32_t order) {
    if ((paddr & (PAGE_SIZE - 1)) || order >= 21) return;
    uint64_t pfn = paddr / PAGE_SIZE;
    uint64_t count = 1ULL << order;
    uint64_t flags;
    spin_lock_irqsave(&pmm_lock, &flags);
    for (uint64_t i = 0; i < count; i++) {
        if (!allocated_page(pfn + i) || g_pages[pfn + i].order != order) {
            spin_unlock_irqrestore(&pmm_lock, flags);
            kprintf("[PMM] Invalid release at 0x%lx (order %u).\n", paddr, order);
            return;
        }
    }
    for (uint64_t i = 0; i < count; i++) {
        page_t *page = &g_pages[pfn + i];
        if (--page->refcount == 0) {
            page->flags = PAGE_FLAG_FREE;
            bitmap_clear(pfn + i);
            g_free_pages++;
            g_used_pages--;
        }
    }
    spin_unlock_irqrestore(&pmm_lock, flags);
}

void pmm_free_page(uint64_t paddr) {
    pmm_free_pages(paddr, 0);
}

void pmm_page_retain(uint64_t paddr) {
    uint64_t pfn = paddr / PAGE_SIZE;
    uint64_t flags;
    spin_lock_irqsave(&pmm_lock, &flags);
    kassert(allocated_page(pfn) && g_pages[pfn].refcount < (uint32_t)-1);
    g_pages[pfn].refcount++;
    spin_unlock_irqrestore(&pmm_lock, flags);
}

void pmm_page_release(uint64_t paddr) {
    pmm_free_page(paddr);
}

uint32_t pmm_page_refcount(uint64_t paddr) {
    uint64_t pfn = paddr / PAGE_SIZE;
    uint64_t flags;
    spin_lock_irqsave(&pmm_lock, &flags);
    uint32_t count = allocated_page(pfn) ? g_pages[pfn].refcount : 0;
    spin_unlock_irqrestore(&pmm_lock, flags);
    return count;
}

uint64_t pmm_get_total_pages(void) {
    return g_total_pages;
}

uint64_t pmm_get_free_pages(void) {
    uint64_t flags;
    spin_lock_irqsave(&pmm_lock, &flags);
    uint64_t pages = g_free_pages;
    spin_unlock_irqrestore(&pmm_lock, flags);
    return pages;
}

uint64_t pmm_get_used_pages(void) {
    uint64_t flags;
    spin_lock_irqsave(&pmm_lock, &flags);
    uint64_t pages = g_used_pages;
    spin_unlock_irqrestore(&pmm_lock, flags);
    return pages;
}
