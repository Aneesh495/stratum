#include <kernel/user_elf.h>
#include <kernel/elf64.h>
#include <kernel/pmm.h>
#include <kernel/vmm.h>
#include <kernel/kernel.h>
#include <kernel/string.h>
#include <shared/errno.h>

int user_elf_load(const void *elf_data, size_t elf_size, user_program_t *out_prog) {
    if (!elf_data || elf_size < sizeof(Elf64_Ehdr) || !out_prog) {
        return -STRATUM_ENOEXEC;
    }

    Elf64_Ehdr header;
    memcpy(&header, elf_data, sizeof(header));
    const Elf64_Ehdr *ehdr = &header;

    /* Validate ELF magic */
    if (ehdr->e_ident[EI_MAG0] != ELFMAG0 ||
        ehdr->e_ident[EI_MAG1] != ELFMAG1 ||
        ehdr->e_ident[EI_MAG2] != ELFMAG2 ||
        ehdr->e_ident[EI_MAG3] != ELFMAG3) {
        return -STRATUM_ENOEXEC;
    }

    /* Validate 64-bit x86-64 executable */
    if (ehdr->e_ident[EI_CLASS] != ELFCLASS64 ||
        ehdr->e_ident[EI_DATA] != ELFDATA2LSB ||
        ehdr->e_machine != EM_X86_64 || ehdr->e_ident[EI_VERSION] != EV_CURRENT ||
        ehdr->e_version != EV_CURRENT || ehdr->e_ehsize != sizeof(*ehdr)) {
        return -STRATUM_ENOEXEC;
    }

    if (ehdr->e_type != ET_EXEC) {
        return -STRATUM_ENOEXEC;
    }

    uint64_t ph_table_size = (uint64_t)ehdr->e_phnum * sizeof(Elf64_Phdr);
    if (ehdr->e_phnum == 0 || ehdr->e_phentsize != sizeof(Elf64_Phdr) ||
        ehdr->e_phoff < sizeof(*ehdr) || ehdr->e_phoff > elf_size || ph_table_size > elf_size - ehdr->e_phoff) {
        return -STRATUM_ENOEXEC;
    }

    /* 1. Allocate isolated user virtual address space */
    pml4_t *pml4 = vmm_create_address_space();
    if (!pml4) {
        return -STRATUM_ENOMEM;
    }

    const uint8_t *phdrs = (const uint8_t *)elf_data + ehdr->e_phoff;

    uint64_t stack_base = USER_STACK_TOP - (USER_STACK_PAGES * PAGE_SIZE);
    uint64_t image_limit = 0x0000400000000000ULL; /* Below the anonymous mapping arena. */
    uint64_t pages_total = 0;
    bool entry_is_executable = false;
    /* 2. Load PT_LOAD segments */
    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        Elf64_Phdr segment;
        memcpy(&segment, phdrs + i * sizeof(segment), sizeof(segment));
        const Elf64_Phdr *ph = &segment;
        if (ph->p_type == PT_INTERP || ph->p_type == PT_DYNAMIC) {
            vmm_destroy_address_space(pml4);
            return -STRATUM_ENOEXEC;
        }
        if (ph->p_type != PT_LOAD) continue;

        if (ph->p_memsz == 0 && ph->p_filesz == 0) continue;

        if (ph->p_vaddr < PAGE_SIZE || ph->p_vaddr >= image_limit ||
            ph->p_memsz > image_limit - ph->p_vaddr ||
            (ph->p_flags & (PF_W | PF_X)) == (PF_W | PF_X)) {
            vmm_destroy_address_space(pml4);
            return -STRATUM_ENOEXEC;
        }

        if (ph->p_filesz > ph->p_memsz || ph->p_offset > elf_size ||
            ph->p_filesz > elf_size - ph->p_offset) {
            vmm_destroy_address_space(pml4);
            return -STRATUM_ENOEXEC;
        }

        uint64_t seg_start = ph->p_vaddr;
        uint64_t seg_end   = ph->p_vaddr + ph->p_memsz;
        if ((ph->p_flags & PF_X) && ehdr->e_entry >= seg_start && ehdr->e_entry - seg_start < ph->p_filesz) {
            entry_is_executable = true;
        }
        uint64_t page_start = seg_start & ~0xFFFULL;
        uint64_t page_end   = (seg_end + 0xFFFULL) & ~0xFFFULL;

        pages_total += (page_end - page_start) / PAGE_SIZE;
        if (pages_total > 4096) {
            vmm_destroy_address_space(pml4);
            return -STRATUM_ENOEXEC;
        }
        uint64_t flags = PTE_PRESENT | PTE_USER;
        if (ph->p_flags & PF_W) flags |= PTE_WRITABLE;
        if (!(ph->p_flags & PF_X)) flags |= PTE_NX;

        for (uint64_t va = page_start; va < page_end; va += PAGE_SIZE) {
            uint64_t paddr = 0, cur_flags = 0;
            if (vmm_get_mapping(pml4, va, &paddr, &cur_flags) != 0) {
                paddr = pmm_alloc_page();
                if (!paddr) {
                    vmm_destroy_address_space(pml4);
                    return -STRATUM_ENOMEM;
                }
                memset(phys_to_virt(paddr), 0, PAGE_SIZE);
                if (vmm_map_page(pml4, va, paddr, flags) != 0) {
                    pmm_free_page(paddr);
                    vmm_destroy_address_space(pml4);
                    return -STRATUM_ENOMEM;
                }
            } else {
                vmm_destroy_address_space(pml4);
                return -STRATUM_ENOEXEC;
            }

            /* Copy file slice into page */
            uint64_t page_va_start = va;
            uint64_t page_va_end   = va + PAGE_SIZE;

            uint64_t copy_va_start = (seg_start > page_va_start) ? seg_start : page_va_start;
            uint64_t copy_va_end   = ((ph->p_vaddr + ph->p_filesz) < page_va_end) ?
                                     (ph->p_vaddr + ph->p_filesz) : page_va_end;

            if (copy_va_end > copy_va_start) {
                uint64_t file_offset = ph->p_offset + (copy_va_start - seg_start);
                uint64_t page_offset = copy_va_start - page_va_start;
                size_t   copy_len    = copy_va_end - copy_va_start;

                memcpy((uint8_t *)phys_to_virt(paddr) + page_offset,
                       (const uint8_t *)elf_data + file_offset,
                       copy_len);
            }
        }
    }

    if (!entry_is_executable) {
        vmm_destroy_address_space(pml4);
        return -STRATUM_ENOEXEC;
    }
    /* 3. Allocate and map user stack */
    for (uint64_t va = stack_base; va < USER_STACK_TOP; va += PAGE_SIZE) {
        uint64_t stack_paddr = pmm_alloc_page();
        if (!stack_paddr) {
            vmm_destroy_address_space(pml4);
            return -STRATUM_ENOMEM;
        }
        memset(phys_to_virt(stack_paddr), 0, PAGE_SIZE);
        if (vmm_map_page(pml4, va, stack_paddr, PTE_USER | PTE_WRITABLE | PTE_NX) != 0) {
            pmm_free_page(stack_paddr);
            vmm_destroy_address_space(pml4);
            return -STRATUM_ENOMEM;
        }
    }

    out_prog->entry_point    = ehdr->e_entry;
    out_prog->user_stack_top = USER_STACK_TOP - 48;
    out_prog->address_space  = pml4;

    return 0;
}
