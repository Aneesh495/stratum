#include <kernel/boot.h>
#include <kernel/pmm.h>

static bool physical_range(uint64_t base, uint64_t length) {
    return base && base < STRATUM_BOOT_PHYS_LIMIT && length &&
           length <= STRATUM_BOOT_PHYS_LIMIT - base;
}

static bool intersects(uint64_t a, uint64_t a_len, uint64_t b, uint64_t b_len) {
    return a_len && b_len && a < b + b_len && b < a + a_len;
}

int boot_check_header(const boot_handoff_t *handoff, uint64_t physical, uint64_t magic) {
    if (magic != STRATUM_BOOT_MAGIC) {
        return -1;
    }
    if (!physical_range(physical, sizeof(*handoff))) {
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

    return 0;
}

int boot_check_descriptors(const boot_handoff_t *handoff, uint64_t physical, const boot_mem_desc_t *entries) {
    uint64_t map_size = (uint64_t)handoff->mem_map_entries * sizeof(*entries);
    for (uint32_t i = 0; i < handoff->mem_map_entries; i++) {
        const boot_mem_desc_t *desc = &entries[i];
        if (!desc->page_count || (desc->phys_addr & (PAGE_SIZE - 1)) ||
            desc->page_count > ((uint64_t)-1 - desc->phys_addr) / PAGE_SIZE) return -11;
        if (desc->type != BOOT_MEM_USABLE) continue;

        uint64_t size = desc->page_count * PAGE_SIZE;
        if (intersects(desc->phys_addr, size, handoff->kernel_phys_base, handoff->kernel_phys_size) ||
            intersects(desc->phys_addr, size, handoff->initramfs_phys, handoff->initramfs_size) ||
            intersects(desc->phys_addr, size, handoff->mem_map_phys, map_size) ||
            intersects(desc->phys_addr, size, physical, sizeof(*handoff))) return -12;

        for (uint32_t j = 0; j < i; j++) {
            if (entries[j].type == BOOT_MEM_USABLE &&
                intersects(desc->phys_addr, size, entries[j].phys_addr, entries[j].page_count * PAGE_SIZE)) {
                return -12;
            }
        }
    }

    return 0;
}
