#include "elf.h"

#define KERNEL_BASE 0xFFFFFFFF80000000ULL
#define KERNEL_WINDOW (64ULL * 1024 * 1024)
#define PAGE_SIZE 4096ULL

static void efi_memset(void *dest, int val, uint64_t count) {
    uint8_t *d = (uint8_t *)dest;
    uint8_t v = (uint8_t)val;
    while (count--) {
        *d++ = v;
    }
}

int elf_validate_header(const Elf64_Ehdr *ehdr, uint64_t file_size) {
    if (!ehdr) return -1;
    if (file_size < sizeof(Elf64_Ehdr)) return -2;

    if (ehdr->e_ident[EI_MAG0] != ELFMAG0 ||
        ehdr->e_ident[EI_MAG1] != ELFMAG1 ||
        ehdr->e_ident[EI_MAG2] != ELFMAG2 ||
        ehdr->e_ident[EI_MAG3] != ELFMAG3) {
        return -3; /* Invalid ELF magic */
    }

    if (ehdr->e_ident[EI_CLASS] != ELFCLASS64) {
        return -4; /* Not 64-bit */
    }

    if (ehdr->e_ident[EI_DATA] != ELFDATA2LSB) {
        return -5; /* Not little-endian */
    }

    if (ehdr->e_machine != EM_X86_64) {
        return -6; /* Not x86-64 */
    }

    if (ehdr->e_type != ET_EXEC) {
        return -7; /* Not executable */
    }

    if (ehdr->e_version != EV_CURRENT || ehdr->e_ident[EI_VERSION] != EV_CURRENT ||
        ehdr->e_ehsize != sizeof(*ehdr) || ehdr->e_phentsize != sizeof(Elf64_Phdr) ||
        ehdr->e_phnum == 0 || ehdr->e_phoff < sizeof(*ehdr) ||
        ehdr->e_phoff > file_size ||
        (uint64_t)ehdr->e_phnum * sizeof(Elf64_Phdr) > file_size - ehdr->e_phoff) {
        return -8; /* Program headers out of file bounds */
    }

    return 0; /* Valid */
}

EFI_STATUS elf_load_segments(EFI_BOOT_SERVICES *bs, EFI_FILE_PROTOCOL *file, const Elf64_Ehdr *ehdr, uint64_t file_size, elf_loaded_image_t *out_info) {
    uint64_t phdr_table_size = (uint64_t)ehdr->e_phnum * sizeof(Elf64_Phdr);
    Elf64_Phdr *phdrs = NULL;

    EFI_STATUS status = bs->AllocatePool(EfiLoaderData, phdr_table_size, (void **)&phdrs);
    if (status != EFI_SUCCESS) {
        return status;
    }

    /* Seek to program header table */
    status = file->SetPosition(file, ehdr->e_phoff);
    if (status != EFI_SUCCESS) {
        bs->FreePool(phdrs);
        return status;
    }

    uint64_t read_bytes = phdr_table_size;
    status = file->Read(file, &read_bytes, phdrs);
    if (status != EFI_SUCCESS || read_bytes != phdr_table_size) {
        bs->FreePool(phdrs);
        return EFI_LOAD_ERROR;
    }

    uint64_t min_vaddr = UINT64_MAX;
    uint64_t max_vaddr = 0;
    int entry_found = 0;

    for (uint32_t i = 0; i < ehdr->e_phnum; i++) {
        const Elf64_Phdr *ph = &phdrs[i];
        if (ph->p_type == PT_INTERP || ph->p_type == PT_DYNAMIC) goto invalid;
        if (ph->p_type != PT_LOAD) continue;
        if (!ph->p_memsz && !ph->p_filesz) continue;

        if (ph->p_vaddr < KERNEL_BASE || ph->p_vaddr >= KERNEL_BASE + KERNEL_WINDOW ||
            ph->p_memsz > KERNEL_BASE + KERNEL_WINDOW - ph->p_vaddr ||
            ph->p_filesz > ph->p_memsz || ph->p_offset > file_size ||
            ph->p_filesz > file_size - ph->p_offset ||
            (ph->p_flags & (PF_W | PF_X)) == (PF_W | PF_X)) goto invalid;

        uint64_t end = ph->p_vaddr + ph->p_memsz;
        for (uint32_t j = 0; j < i; j++) {
            const Elf64_Phdr *other = &phdrs[j];
            if (other->p_type == PT_LOAD && other->p_memsz &&
                ph->p_vaddr < other->p_vaddr + other->p_memsz && other->p_vaddr < end) {
                goto invalid;
            }
        }
        if (ph->p_vaddr < min_vaddr) min_vaddr = ph->p_vaddr;
        if (end > max_vaddr) max_vaddr = end;
        if ((ph->p_flags & PF_X) && ehdr->e_entry >= ph->p_vaddr &&
            ehdr->e_entry - ph->p_vaddr < ph->p_filesz) entry_found = 1;
    }

    if (!entry_found || min_vaddr != KERNEL_BASE) goto invalid;
    uint64_t page_count = (max_vaddr - min_vaddr + PAGE_SIZE - 1) / PAGE_SIZE;

    uint64_t phys_base = 0;
    status = bs->AllocatePages(AllocateAnyPages, EfiLoaderData, page_count, &phys_base);
    if (status != EFI_SUCCESS) {
        bs->FreePool(phdrs);
        return status;
    }

    /* Zero out the entire allocated memory span */
    efi_memset((void *)phys_base, 0, page_count * 0x1000);

    /* Load each PT_LOAD segment */
    for (uint32_t i = 0; i < ehdr->e_phnum; i++) {
        if (phdrs[i].p_type != PT_LOAD) {
            continue;
        }

        uint64_t seg_offset = phdrs[i].p_vaddr - min_vaddr;
        void *dest = (void *)(phys_base + seg_offset);

        if (phdrs[i].p_filesz > 0) {
            status = file->SetPosition(file, phdrs[i].p_offset);
            if (status != EFI_SUCCESS) {
                bs->FreePages(phys_base, page_count);
                bs->FreePool(phdrs);
                return status;
            }

            uint64_t seg_read = phdrs[i].p_filesz;
            status = file->Read(file, &seg_read, dest);
            if (status != EFI_SUCCESS || seg_read != phdrs[i].p_filesz) {
                bs->FreePages(phys_base, page_count);
                bs->FreePool(phdrs);
                return EFI_LOAD_ERROR;
            }
        }

        /* If memsz > filesz, the rest is zeroed (already done by full span zeroing) */
    }

    bs->FreePool(phdrs);

    out_info->phys_base = phys_base;
    out_info->phys_size = page_count * 0x1000;
    out_info->virt_base = min_vaddr;
    out_info->entry_point = ehdr->e_entry;

    return EFI_SUCCESS;
invalid:
    bs->FreePool(phdrs);
    return EFI_LOAD_ERROR;
}
