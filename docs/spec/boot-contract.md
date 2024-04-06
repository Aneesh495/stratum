# Stratum Boot Contract Specification

Version: 1.0.0
Status: Frozen

## 1. Overview
The Stratum Boot Contract establishes the exact protocol and data structures exchanged between the original UEFI bootloader (`boot/uefi/`) and the Stratum x86-64 kernel (`kernel/`).

## 2. Calling Convention and Initial CPU State
Upon entry from the bootloader to the kernel entry point (`_start`):
- CPU Mode: 64-bit Long Mode with paging enabled.
- CS: 64-bit code segment, selector privileges ring 0.
- DS, ES, SS, FS, GS: 64-bit data segment or null selectors, base 0.
- RFLAGS: Interrupts disabled (IF=0), Direction Flag cleared (DF=0).
- RSP: Valid 16-byte aligned boot stack allocated in reserved physical memory.
- RDI: 64-bit physical address of the `boot_handoff_t` structure.
- RSI: Magic value `0x5354524154554D31` ("STRATUM1" in ASCII).
- Floating-Point: CR0.EM=0, CR0.MP=1, CR4.OSFXSR=1, CR4.OSXMMEXCPT=1. SSE enabled.

## 3. Memory Ownership and Handoff State
Before transferring control to the kernel:
1. The loader calls UEFI `ExitBootServices()`.
2. Firmware runtime services are not retained as kernel dependencies.
3. The loader supplies a sanitized memory map covering all physical RAM descriptors.
4. Memory regions are classified into explicit types:
   - `BOOT_MEM_USABLE`: Free conventional memory available for the physical page allocator.
   - `BOOT_MEM_RESERVED`: Reserved physical RAM (firmware, ACPI, device buffers).
   - `BOOT_MEM_LOADER`: Memory containing loader tables, boot stack, and handoff structures.
   - `BOOT_MEM_KERNEL_IMAGE`: Memory occupied by the kernel ELF64 segments.
   - `BOOT_MEM_INITRAMFS`: Physical range holding the loaded initramfs archive.
   - `BOOT_MEM_FRAMEBUFFER`: Memory mapped for the linear graphics framebuffer.

## 4. Boot Handoff Structure (`boot_handoff_t`)

```c
#define STRATUM_BOOT_MAGIC 0x5354524154554D31
#define STRATUM_BOOT_VERSION 1

typedef struct {
    uint32_t type;         /* BOOT_MEM_* */
    uint32_t flags;
    uint64_t phys_addr;
    uint64_t virt_addr;
    uint64_t page_count;
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

    /* Firmware and ACPI */
    uint64_t rsdp_phys;      /* ACPI Root System Description Pointer */

    /* Memory map */
    uint64_t mem_map_phys;   /* Pointer to array of boot_mem_desc_t */
    uint32_t mem_map_entries;
    uint32_t mem_map_entry_size;

    /* Kernel image boundaries */
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
```

## 5. Validation rules

The handoff, boot page tables, kernel image, stack, and descriptor array are
allocated below 4 GiB. The loader requires a static x86-64 kernel ELF linked
within the 64 MiB kernel load window. It rejects overlapping load segments,
writable-executable segments, out-of-range file slices, and entry points
outside file-backed executable code.

The kernel checks the handoff magic, version, structure size, memory-map
count and stride, physical ranges, framebuffer geometry, and command-line
termination. It rejects usable memory descriptors that overlap one another
or the handoff, kernel image, initramfs, or descriptor array. A rejected
handoff produces a serial diagnostic and halts.
