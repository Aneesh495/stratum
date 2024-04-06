#include <kernel/boot.h>
#include <kernel/kernel.h>
#include <kernel/pmm.h>

boot_handoff_t g_boot_handoff;

static bool physical_range(uint64_t base, uint64_t length) {
    return base && base < STRATUM_BOOT_PHYS_LIMIT && length &&
           length <= STRATUM_BOOT_PHYS_LIMIT - base;
}

static bool intersects(uint64_t a, uint64_t a_len, uint64_t b, uint64_t b_len) {
    return a_len && b_len && a < b + b_len && b < a + a_len;
}

int boot_validate_handoff(boot_handoff_t *handoff, uint64_t magic) {
    if (magic != STRATUM_BOOT_MAGIC) {
        return -1;
    }
    if (!physical_range((uint64_t)handoff, sizeof(*handoff))) {
        return -2;
    }
    if (handoff->magic != STRATUM_BOOT_MAGIC) {
        return -3;
    }
    if (handoff->version != STRATUM_BOOT_VERSION) {
        return -4;
    }
    if (handoff->header_size != sizeof(boot_handoff_t)) {
        return -5;
    }
    uint64_t map_size = (uint64_t)handoff->mem_map_entries * sizeof(boot_mem_desc_t);
    if (!handoff->mem_map_entries || handoff->mem_map_entries > STRATUM_BOOT_MAX_DESCS ||
        handoff->mem_map_entry_size != sizeof(boot_mem_desc_t) ||
        !physical_range(handoff->mem_map_phys, map_size)) return -6;

    if (!physical_range(handoff->kernel_phys_base, handoff->kernel_phys_size) ||
        (handoff->kernel_phys_base & (PAGE_SIZE - 1)) ||
        (handoff->kernel_phys_size & (PAGE_SIZE - 1)) ||
        handoff->kernel_phys_size > STRATUM_KERNEL_WINDOW ||
        handoff->kernel_virt_base != STRATUM_KERNEL_BASE ||
        handoff->kernel_entry_virt < handoff->kernel_virt_base ||
        handoff->kernel_entry_virt - handoff->kernel_virt_base >= handoff->kernel_phys_size) return -7;

    if ((handoff->initramfs_size && !physical_range(handoff->initramfs_phys, handoff->initramfs_size)) ||
        (handoff->rsdp_phys && !physical_range(handoff->rsdp_phys, 36))) return -8;
    if (handoff->fb_base_phys) {
        if (!handoff->fb_width || !handoff->fb_height || handoff->fb_stride < handoff->fb_width ||
            handoff->fb_stride > 16384 || handoff->fb_height > 16384 ||
            (handoff->fb_format != 1 && handoff->fb_format != 2) ||
            !physical_range(handoff->fb_base_phys, (uint64_t)handoff->fb_stride * handoff->fb_height * 4)) {
            return -9;
        }
    }
    if (handoff->cmdline[sizeof(handoff->cmdline) - 1] != 0) return -10;

    const boot_mem_desc_t *entries = (const boot_mem_desc_t *)handoff->mem_map_phys;
    for (uint32_t i = 0; i < handoff->mem_map_entries; i++) {
        const boot_mem_desc_t *desc = &entries[i];
        if (!desc->page_count || (desc->phys_addr & (PAGE_SIZE - 1)) ||
            desc->page_count > ((uint64_t)-1 - desc->phys_addr) / PAGE_SIZE) return -11;
        if (desc->type != BOOT_MEM_USABLE) continue;

        uint64_t size = desc->page_count * PAGE_SIZE;
        if (intersects(desc->phys_addr, size, handoff->kernel_phys_base, handoff->kernel_phys_size) ||
            intersects(desc->phys_addr, size, handoff->initramfs_phys, handoff->initramfs_size) ||
            intersects(desc->phys_addr, size, handoff->mem_map_phys, map_size) ||
            intersects(desc->phys_addr, size, (uint64_t)handoff, sizeof(*handoff))) return -12;

        for (uint32_t j = 0; j < i; j++) {
            if (entries[j].type == BOOT_MEM_USABLE &&
                intersects(desc->phys_addr, size, entries[j].phys_addr, entries[j].page_count * PAGE_SIZE)) {
                return -12;
            }
        }
    }

    /* Copy handoff into kernel global storage */
    memcpy(&g_boot_handoff, handoff, sizeof(boot_handoff_t));
    return 0;
}

void boot_dump_info(const boot_handoff_t *handoff) {
    kprintf("[BOOT] Magic: 0x%lx (valid)\n", handoff->magic);
    kprintf("[BOOT] Version: %u, Header Size: %u bytes\n", handoff->version, handoff->header_size);
    kprintf("[BOOT] Kernel Phys: 0x%lx - 0x%lx (size: %lu KiB)\n",
            handoff->kernel_phys_base,
            handoff->kernel_phys_base + handoff->kernel_phys_size,
            handoff->kernel_phys_size / 1024);
    kprintf("[BOOT] Kernel Virt Base: 0x%lx, Entry: 0x%lx\n",
            handoff->kernel_virt_base, handoff->kernel_entry_virt);

    if (handoff->initramfs_size > 0) {
        kprintf("[BOOT] Initramfs Phys: 0x%lx (size: %lu bytes)\n",
                handoff->initramfs_phys, handoff->initramfs_size);
    } else {
        kprintf("[BOOT] Initramfs: none detected\n");
    }

    if (handoff->fb_base_phys != 0) {
        kprintf("[BOOT] Framebuffer: %ux%u stride=%u at 0x%lx (format=%u)\n",
                handoff->fb_width, handoff->fb_height, handoff->fb_stride,
                handoff->fb_base_phys, handoff->fb_format);
    }

    if (handoff->rsdp_phys != 0) {
        kprintf("[BOOT] ACPI RSDP Pointer: 0x%lx\n", handoff->rsdp_phys);
    }

    /* Summarize memory map */
    boot_mem_desc_t *descs = (boot_mem_desc_t *)handoff->mem_map_phys;
    uint64_t usable_bytes = 0;
    uint64_t reserved_bytes = 0;

    for (uint32_t i = 0; i < handoff->mem_map_entries; i++) {
        uint64_t bytes = descs[i].page_count * 4096;
        if (descs[i].type == BOOT_MEM_USABLE) {
            usable_bytes += bytes;
        } else {
            reserved_bytes += bytes;
        }
    }

    kprintf("[BOOT] Memory Map: %u descriptors, %lu MiB usable RAM, %lu MiB reserved\n",
            handoff->mem_map_entries,
            usable_bytes / (1024 * 1024),
            reserved_bytes / (1024 * 1024));
}
