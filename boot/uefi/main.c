#include <boot/efi.h>
#include "elf.h"
#include <shared/boot_info.h>

#define PAGE_SIZE 4096

/* Page Table Entry Flags */
#define PTE_PRESENT   (1ULL << 0)
#define PTE_WRITABLE  (1ULL << 1)
#define PTE_USER      (1ULL << 2)
#define PTE_HUGE      (1ULL << 7)
#define PTE_NX        (1ULL << 63)

static void efi_print(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *con, const char *ascii) {
    if (!con || !ascii) return;
    int16_t u16[2];
    u16[1] = 0;
    while (*ascii) {
        if (*ascii == '\n') {
            u16[0] = '\r';
            con->OutputString(con, u16);
        }
        u16[0] = (int16_t)*ascii++;
        con->OutputString(con, u16);
    }
}

static uint64_t guid_cmp(const EFI_GUID *a, const EFI_GUID *b) {
    if (a->Data1 != b->Data1 || a->Data2 != b->Data2 || a->Data3 != b->Data3) {
        return 1;
    }
    for (int i = 0; i < 8; i++) {
        if (a->Data4[i] != b->Data4[i]) return 1;
    }
    return 0;
}

static void efi_zero(void *dest, uint64_t bytes) {
    uint8_t *d = (uint8_t *)dest;
    while (bytes--) {
        *d++ = 0;
    }
}

static EFI_STATUS allocate_boot_pages(EFI_BOOT_SERVICES *bs, uint64_t pages, uint64_t *address) {
    *address = STRATUM_BOOT_PHYS_LIMIT - 1;
    return bs->AllocatePages(AllocateMaxAddress, EfiLoaderData, pages, address);
}

/* Construct 4-level page tables that map identity 0..4GB and high kernel */
static uint64_t setup_boot_page_tables(EFI_BOOT_SERVICES *bs, uint64_t kernel_phys_base, uint64_t kernel_virt_base, uint64_t kernel_pages) {
    uint64_t pml4_phys = 0;
    EFI_STATUS st = allocate_boot_pages(bs, 1, &pml4_phys);
    if (st != EFI_SUCCESS) return 0;
    efi_zero((void *)pml4_phys, PAGE_SIZE);

    /* 1. Identity Map first 4 GiB using 2 MiB large pages (PML4[0]) */
    uint64_t id_pdpt_phys = 0;
    st = allocate_boot_pages(bs, 1, &id_pdpt_phys);
    if (st != EFI_SUCCESS) return 0;
    efi_zero((void *)id_pdpt_phys, PAGE_SIZE);

    uint64_t *pml4 = (uint64_t *)pml4_phys;
    pml4[0] = id_pdpt_phys | PTE_PRESENT | PTE_WRITABLE;

    /* Also map direct physical map at PML4[256] -> 0xFFFF800000000000 */
    pml4[256] = id_pdpt_phys | PTE_PRESENT | PTE_WRITABLE;

    uint64_t *id_pdpt = (uint64_t *)id_pdpt_phys;
    for (uint64_t g = 0; g < 4; g++) {
        uint64_t pd_phys = 0;
        st = allocate_boot_pages(bs, 1, &pd_phys);
        if (st != EFI_SUCCESS) return 0;
        efi_zero((void *)pd_phys, PAGE_SIZE);

        id_pdpt[g] = pd_phys | PTE_PRESENT | PTE_WRITABLE;

        uint64_t *pd = (uint64_t *)pd_phys;
        for (uint64_t m = 0; m < 512; m++) {
            uint64_t phys_addr = (g * 512 + m) * 0x200000ULL; /* 2 MiB */
            pd[m] = phys_addr | PTE_PRESENT | PTE_WRITABLE | PTE_HUGE;
        }
    }

    /* 2. Map High Kernel at PML4[511] (0xFFFFFFFF80000000) */
    /* Virtual address 0xFFFFFFFF80000000:
       PML4 index = (0xFFFFFFFF80000000 >> 39) & 0x1FF = 511
       PDPT index = (0xFFFFFFFF80000000 >> 30) & 0x1FF = 510
       PD index   = (0xFFFFFFFF80000000 >> 21) & 0x1FF = 0
    */
    uint64_t high_pdpt_phys = 0;
    st = allocate_boot_pages(bs, 1, &high_pdpt_phys);
    if (st != EFI_SUCCESS) return 0;
    efi_zero((void *)high_pdpt_phys, PAGE_SIZE);

    pml4[511] = high_pdpt_phys | PTE_PRESENT | PTE_WRITABLE;

    uint64_t high_pd_phys = 0;
    st = allocate_boot_pages(bs, 1, &high_pd_phys);
    if (st != EFI_SUCCESS) return 0;
    efi_zero((void *)high_pd_phys, PAGE_SIZE);

    uint64_t *high_pdpt = (uint64_t *)high_pdpt_phys;
    high_pdpt[510] = high_pd_phys | PTE_PRESENT | PTE_WRITABLE;

    /* Map kernel using 4 KiB pages */
    uint64_t *high_pd = (uint64_t *)high_pd_phys;
    uint64_t pt_count = (kernel_pages + 511) / 512;
    for (uint64_t pti = 0; pti < pt_count; pti++) {
        uint64_t pt_phys = 0;
        st = allocate_boot_pages(bs, 1, &pt_phys);
        if (st != EFI_SUCCESS) return 0;
        efi_zero((void *)pt_phys, PAGE_SIZE);

        high_pd[pti] = pt_phys | PTE_PRESENT | PTE_WRITABLE;

        uint64_t *pt = (uint64_t *)pt_phys;
        for (uint64_t e = 0; e < 512; e++) {
            uint64_t p_index = pti * 512 + e;
            if (p_index < kernel_pages) {
                pt[e] = (kernel_phys_base + p_index * PAGE_SIZE) | PTE_PRESENT | PTE_WRITABLE;
            }
        }
    }

    (void)kernel_virt_base;
    return pml4_phys;
}

EFI_STATUS EfiMain(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    EFI_BOOT_SERVICES *bs = SystemTable->BootServices;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *con = SystemTable->ConOut;

    if (con) {
        con->ClearScreen(con);
        efi_print(con, "========================================\n");
        efi_print(con, "  Stratum Multiprocessor OS UEFI Loader\n");
        efi_print(con, "========================================\n");
    }

    /* 1. Allocate Handoff Structure */
    uint64_t handoff_phys = 0;
    EFI_STATUS st = allocate_boot_pages(bs, 1, &handoff_phys);
    if (st != EFI_SUCCESS) {
        efi_print(con, "ERROR: Failed to allocate boot handoff memory.\n");
        return st;
    }
    boot_handoff_t *handoff = (boot_handoff_t *)handoff_phys;
    efi_zero(handoff, sizeof(boot_handoff_t));

    handoff->magic = STRATUM_BOOT_MAGIC;
    handoff->version = STRATUM_BOOT_VERSION;
    handoff->header_size = sizeof(boot_handoff_t);

    /* 2. Locate LoadedImageProtocol and SimpleFileSystem */
    EFI_LOADED_IMAGE_PROTOCOL *loaded_image = NULL;
    st = bs->HandleProtocol(ImageHandle, &EFI_LOADED_IMAGE_PROTOCOL_GUID, (void **)&loaded_image);
    if (st != EFI_SUCCESS) {
        efi_print(con, "ERROR: Cannot get LoadedImageProtocol.\n");
        return st;
    }

    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = NULL;
    st = bs->HandleProtocol(loaded_image->DeviceHandle, &EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID, (void **)&fs);
    if (st != EFI_SUCCESS) {
        efi_print(con, "ERROR: Cannot open SimpleFileSystem.\n");
        return st;
    }

    EFI_FILE_PROTOCOL *root_dir = NULL;
    st = fs->OpenVolume(fs, &root_dir);
    if (st != EFI_SUCCESS) {
        efi_print(con, "ERROR: Cannot open root volume.\n");
        return st;
    }

    /* 3. Open and Load Kernel ELF Image */
    EFI_FILE_PROTOCOL *kernel_file = NULL;
    const int16_t kernel_path[] = { '\\', 's', 't', 'r', 'a', 't', 'u', 'm', '.', 'e', 'l', 'f', 0 };
    st = root_dir->Open(root_dir, &kernel_file, kernel_path, EFI_FILE_MODE_READ, EFI_FILE_READ_ONLY);
    if (st != EFI_SUCCESS) {
        efi_print(con, "ERROR: Kernel image \\stratum.elf not found.\n");
        return st;
    }

    /* Read ELF Header */
    Elf64_Ehdr ehdr;
    uint64_t ehdr_size = sizeof(Elf64_Ehdr);
    st = kernel_file->Read(kernel_file, &ehdr_size, &ehdr);
    if (st != EFI_SUCCESS || ehdr_size != sizeof(Elf64_Ehdr)) {
        efi_print(con, "ERROR: Failed to read kernel ELF header.\n");
        return EFI_LOAD_ERROR;
    }

    /* Get File Info to determine file size */
    uint8_t info_buf[256];
    uint64_t info_size = sizeof(info_buf);
    st = kernel_file->GetInfo(kernel_file, &EFI_FILE_INFO_ID, &info_size, info_buf);
    if (st != EFI_SUCCESS) {
        efi_print(con, "ERROR: Failed to determine kernel image length.\n");
        kernel_file->Close(kernel_file);
        return st;
    }
    uint64_t kernel_file_size = ((EFI_FILE_INFO *)info_buf)->FileSize;

    int val_err = elf_validate_header(&ehdr, kernel_file_size);
    if (val_err != 0) {
        efi_print(con, "ERROR: Kernel ELF header validation failed.\n");
        return EFI_LOAD_ERROR;
    }

    elf_loaded_image_t loaded_kernel;
    st = elf_load_segments(bs, kernel_file, &ehdr, kernel_file_size, &loaded_kernel);
    if (st != EFI_SUCCESS) {
        efi_print(con, "ERROR: Failed to load kernel ELF segments.\n");
        return st;
    }
    kernel_file->Close(kernel_file);

    handoff->kernel_phys_base = loaded_kernel.phys_base;
    handoff->kernel_phys_size = loaded_kernel.phys_size;
    handoff->kernel_virt_base = loaded_kernel.virt_base;
    handoff->kernel_entry_virt = loaded_kernel.entry_point;

    efi_print(con, "Loaded kernel ELF into physical RAM.\n");

    /* 4. Check for Initramfs */
    EFI_FILE_PROTOCOL *initramfs_file = NULL;
    const int16_t initramfs_path[] = { '\\', 'i', 'n', 'i', 't', 'r', 'a', 'm', 'f', 's', '.', 'c', 'p', 'i', 'o', 0 };
    st = root_dir->Open(root_dir, &initramfs_file, initramfs_path, EFI_FILE_MODE_READ, EFI_FILE_READ_ONLY);
    if (st == EFI_SUCCESS) {
        info_size = sizeof(info_buf);
        st = initramfs_file->GetInfo(initramfs_file, &EFI_FILE_INFO_ID, &info_size, info_buf);
        if (st == EFI_SUCCESS) {
            EFI_FILE_INFO *finfo = (EFI_FILE_INFO *)info_buf;
            uint64_t ramfs_size = finfo->FileSize;
            if (ramfs_size && ramfs_size <= 32 * 1024 * 1024) {
                uint64_t ramfs_pages = (ramfs_size + PAGE_SIZE - 1) / PAGE_SIZE;
                uint64_t ramfs_phys;
                st = allocate_boot_pages(bs, ramfs_pages, &ramfs_phys);
                if (st == EFI_SUCCESS) {
                    uint64_t read_ramfs = ramfs_size;
                    st = initramfs_file->Read(initramfs_file, &read_ramfs, (void *)ramfs_phys);
                    if (st == EFI_SUCCESS && read_ramfs == ramfs_size) {
                        handoff->initramfs_phys = ramfs_phys;
                        handoff->initramfs_size = ramfs_size;
                        efi_print(con, "Loaded initramfs archive.\n");
                    } else {
                        bs->FreePages(ramfs_phys, ramfs_pages);
                    }
                }
            }
        }
        initramfs_file->Close(initramfs_file);
    }

    /* Check for cmdline.txt */
    EFI_FILE_PROTOCOL *cmd_file = NULL;
    const int16_t cmd_path[] = { '\\', 'c', 'm', 'd', 'l', 'i', 'n', 'e', '.', 't', 'x', 't', 0 };
    st = root_dir->Open(root_dir, &cmd_file, cmd_path, EFI_FILE_MODE_READ, EFI_FILE_READ_ONLY);
    if (st == EFI_SUCCESS) {
        uint64_t cmd_read = sizeof(handoff->cmdline) - 1;
        st = cmd_file->Read(cmd_file, &cmd_read, handoff->cmdline);
        cmd_file->Close(cmd_file);
        if (st == EFI_SUCCESS && cmd_read < sizeof(handoff->cmdline)) {
            handoff->cmdline[cmd_read] = '\0';
            efi_print(con, "Loaded boot command line.\n");
        } else {
            handoff->cmdline[0] = '\0';
        }
    }
    root_dir->Close(root_dir);

    /* 5. Graphics Output Protocol (GOP) */
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = NULL;
    st = bs->LocateProtocol(&EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID, NULL, (void **)&gop);
    if (st == EFI_SUCCESS && gop && gop->Mode && gop->Mode->Info &&
        (gop->Mode->Info->PixelFormat == PixelBlueGreenRedReserved8BitPerColor ||
         gop->Mode->Info->PixelFormat == PixelRedGreenBlueReserved8BitPerColor)) {
        handoff->fb_base_phys = gop->Mode->FrameBufferBase;
        handoff->fb_width = gop->Mode->Info->HorizontalResolution;
        handoff->fb_height = gop->Mode->Info->VerticalResolution;
        handoff->fb_stride = gop->Mode->Info->PixelsPerScanLine;
        handoff->fb_format = (gop->Mode->Info->PixelFormat == PixelBlueGreenRedReserved8BitPerColor) ? 1 : 2;
    }

    /* 6. ACPI Table Pointer */
    for (uint64_t i = 0; i < SystemTable->NumberOfTableEntries; i++) {
        if (guid_cmp(&SystemTable->ConfigurationTable[i].VendorGuid, &EFI_ACPI_20_TABLE_GUID) == 0 ||
            guid_cmp(&SystemTable->ConfigurationTable[i].VendorGuid, &EFI_ACPI_10_TABLE_GUID) == 0) {
            handoff->rsdp_phys = (uint64_t)SystemTable->ConfigurationTable[i].VendorTable;
            break;
        }
    }

    /* 7. Setup Boot Page Tables */
    uint64_t kpages = (handoff->kernel_phys_size + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t pml4_phys = setup_boot_page_tables(bs, handoff->kernel_phys_base, handoff->kernel_virt_base, kpages);
    if (!pml4_phys) {
        efi_print(con, "ERROR: Failed to allocate initial page tables.\n");
        return EFI_OUT_OF_RESOURCES;
    }

    /* 8. Allocate Boot Memory Map Descriptors */
    uint64_t boot_mem_descs_phys = 0;
    st = allocate_boot_pages(bs, (STRATUM_BOOT_MAX_DESCS * sizeof(boot_mem_desc_t) + PAGE_SIZE - 1) / PAGE_SIZE, &boot_mem_descs_phys);
    if (st != EFI_SUCCESS) {
        return st;
    }
    boot_mem_desc_t *boot_mem_descs = (boot_mem_desc_t *)boot_mem_descs_phys;

    /* Allocate Boot Stack (64 KiB) */
    uint64_t boot_stack_phys = 0;
    st = allocate_boot_pages(bs, 16, &boot_stack_phys);
    if (st != EFI_SUCCESS) {
        return st;
    }
    uint64_t boot_stack_top = boot_stack_phys + 16 * PAGE_SIZE;

    /* 9. ExitBootServices Loop */
    efi_print(con, "Terminating UEFI boot services and entering Stratum kernel...\n");

    uint64_t mem_map_size = 0;
    EFI_MEMORY_DESCRIPTOR *efi_mmap = NULL;
    uint64_t map_key = 0;
    uint64_t desc_size = 0;
    uint32_t desc_version = 0;

    /* Query size first */
    bs->GetMemoryMap(&mem_map_size, NULL, &map_key, &desc_size, &desc_version);
    uint64_t map_capacity = mem_map_size + 4096;
    st = bs->AllocatePool(EfiLoaderData, map_capacity, (void **)&efi_mmap);
    if (st != EFI_SUCCESS) {
        return st;
    }

    int attempts = 0;
    while (attempts++ < 5) {
        mem_map_size = map_capacity;
        st = bs->GetMemoryMap(&mem_map_size, efi_mmap, &map_key, &desc_size, &desc_version);
        if (st != EFI_SUCCESS) {
            continue;
        }

        if (desc_size < sizeof(EFI_MEMORY_DESCRIPTOR) || mem_map_size % desc_size != 0 ||
            mem_map_size / desc_size > STRATUM_BOOT_MAX_DESCS) return EFI_LOAD_ERROR;

        st = bs->ExitBootServices(ImageHandle, map_key);
        if (st == EFI_SUCCESS) {
            break;
        }
    }

    if (st != EFI_SUCCESS) {
        /* Could not exit boot services */
        return st;
    }

    /* --- WE ARE NOW POST-EXITBOOTSERVICES --- */
    /* Translate memory map into handoff */
    uint32_t out_count = 0;
    uint64_t num_entries = mem_map_size / desc_size;
    uint8_t *cur = (uint8_t *)efi_mmap;

    for (uint64_t i = 0; i < num_entries; i++) {
        EFI_MEMORY_DESCRIPTOR *d = (EFI_MEMORY_DESCRIPTOR *)cur;
        boot_mem_desc_t *out = &boot_mem_descs[out_count];

        out->phys_addr = d->PhysicalStart;
        out->virt_addr = d->VirtualStart;
        out->page_count = d->NumberOfPages;
        out->flags = (uint32_t)d->Attribute;

        switch (d->Type) {
            case EfiConventionalMemory:
                out->type = BOOT_MEM_USABLE;
                break;
            case EfiLoaderCode:
            case EfiLoaderData:
            case EfiBootServicesCode:
            case EfiBootServicesData:
                out->type = BOOT_MEM_LOADER;
                break;
            case EfiACPIReclaimMemory:
            case EfiACPIMemoryNVS:
                out->type = BOOT_MEM_ACPI;
                break;
            default:
                out->type = BOOT_MEM_RESERVED;
                break;
        }

        out_count++;
        cur += desc_size;
    }

    handoff->mem_map_phys = boot_mem_descs_phys;
    handoff->mem_map_entries = out_count;
    handoff->mem_map_entry_size = sizeof(boot_mem_desc_t);

    /* 10. Switch CR3 and Jump to Kernel */
    uint64_t entry_addr = handoff->kernel_entry_virt;

    /*
     * We transition stack to boot_stack_top, load CR3 with pml4_phys,
     * place handoff_phys into RDI, STRATUM_BOOT_MAGIC into RSI, and jump to entry_addr.
     */
    __asm__ volatile(
        "cli\n\t"
        "cld\n\t"
        "movq %0, %%cr3\n\t"
        "movq %1, %%rsp\n\t"
        "movq %2, %%rdi\n\t"
        "movq %3, %%rsi\n\t"
        "jmpq *%4\n\t"
        :
        : "r"(pml4_phys),
          "r"(boot_stack_top),
          "r"(handoff_phys),
          "r"(STRATUM_BOOT_MAGIC),
          "r"(entry_addr)
        : "rdi", "rsi", "memory"
    );

    /* Unreachable */
    while (1) {
        __asm__ volatile("hlt");
    }
}
