#ifndef STRATUM_SHARED_BOOT_INFO_H
#define STRATUM_SHARED_BOOT_INFO_H

#if defined(__has_include)
  #if __has_include(<kernel/types.h>)
    #include <kernel/types.h>
  #else
    #include <stdint.h>
  #endif
#else
  #include <stdint.h>
#endif

#define STRATUM_BOOT_MAGIC   0x5354524154554D31ULL /* "STRATUM1" */
#define STRATUM_BOOT_VERSION 1
#define STRATUM_BOOT_MAX_DESCS 512
#define STRATUM_BOOT_PHYS_LIMIT (1ULL << 32)
#define STRATUM_KERNEL_BASE 0xFFFFFFFF80000000ULL
#define STRATUM_KERNEL_WINDOW (64ULL * 1024 * 1024)

/* Memory region types passed by the bootloader */
#define BOOT_MEM_USABLE        1  /* Free conventional RAM for PMM */
#define BOOT_MEM_RESERVED      2  /* Reserved by firmware/hardware */
#define BOOT_MEM_LOADER        3  /* Bootloader stack/tables, reclaimable later */
#define BOOT_MEM_KERNEL_IMAGE  4  /* Memory holding loaded kernel segments */
#define BOOT_MEM_INITRAMFS     5  /* Memory holding the loaded initramfs image */
#define BOOT_MEM_FRAMEBUFFER   6  /* Memory-mapped graphics buffer */
#define BOOT_MEM_ACPI          7  /* ACPI tables */

typedef struct {
    uint32_t type;         /* BOOT_MEM_* */
    uint32_t flags;
    uint64_t phys_addr;
    uint64_t virt_addr;
    uint64_t page_count;   /* Number of 4 KiB pages */
} boot_mem_desc_t;

typedef struct {
    uint64_t magic;          /* STRATUM_BOOT_MAGIC */
    uint32_t version;        /* STRATUM_BOOT_VERSION */
    uint32_t header_size;    /* sizeof(boot_handoff_t) */

    /* Framebuffer metadata */
    uint64_t fb_base_phys;
    uint32_t fb_width;
    uint32_t fb_height;
    uint32_t fb_stride;
    uint32_t fb_format;      /* 1 = BGRX8888, 2 = RGBX8888 */

    /* ACPI root system description pointer */
    uint64_t rsdp_phys;

    /* Memory map */
    uint64_t mem_map_phys;   /* Pointer to array of boot_mem_desc_t */
    uint32_t mem_map_entries;
    uint32_t mem_map_entry_size;

    /* Kernel physical and virtual boundaries */
    uint64_t kernel_phys_base;
    uint64_t kernel_phys_size;
    uint64_t kernel_virt_base;
    uint64_t kernel_entry_virt;

    /* Initramfs */
    uint64_t initramfs_phys;
    uint64_t initramfs_size;

    /* Boot command line */
    char cmdline[256];
} boot_handoff_t;

#endif /* STRATUM_SHARED_BOOT_INFO_H */
