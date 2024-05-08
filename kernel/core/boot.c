#include <kernel/boot.h>
#include <kernel/kernel.h>
#include <kernel/pmm.h>

boot_handoff_t g_boot_handoff;

int boot_validate_handoff(boot_handoff_t *handoff, uint64_t magic) {
    uint64_t physical = (uint64_t)handoff;
    int result = boot_check_header(handoff, physical, magic);
    if (result) return result;

    result = boot_check_descriptors(handoff, physical, (const boot_mem_desc_t *)handoff->mem_map_phys);
    if (result) return result;

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
