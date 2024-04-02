#ifndef STRATUM_BOOT_ELF_H
#define STRATUM_BOOT_ELF_H

#include <stdint.h>
#include <boot/efi.h>

#define EI_MAG0       0
#define EI_MAG1       1
#define EI_MAG2       2
#define EI_MAG3       3
#define EI_CLASS      4
#define EI_DATA       5
#define EI_VERSION    6
#define EI_OSABI      7
#define EI_ABIVERSION 8
#define EI_NIDENT     16

#define ELFMAG0       0x7F
#define ELFMAG1       'E'
#define ELFMAG2       'L'
#define ELFMAG3       'F'

#define ELFCLASS64    2
#define ELFDATA2LSB   1
#define EV_CURRENT    1
#define ET_EXEC       2
#define ET_DYN        3
#define EM_X86_64     62

#define PT_NULL       0
#define PT_LOAD       1
#define PT_DYNAMIC    2
#define PT_INTERP     3
#define PT_NOTE       4
#define PT_SHLIB      5
#define PT_PHDR       6
#define PT_TLS        7

#define PF_X          1
#define PF_W          2
#define PF_R          4

typedef struct {
    uint8_t  e_ident[EI_NIDENT];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} Elf64_Ehdr;

typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} Elf64_Phdr;

typedef struct {
    uint64_t phys_base;
    uint64_t phys_size;
    uint64_t virt_base;
    uint64_t entry_point;
} elf_loaded_image_t;

/* Validation and loading functions */
int elf_validate_header(const Elf64_Ehdr *ehdr, uint64_t file_size);
EFI_STATUS elf_load_segments(EFI_BOOT_SERVICES *bs, EFI_FILE_PROTOCOL *file, const Elf64_Ehdr *ehdr, uint64_t file_size, elf_loaded_image_t *out_info);

#endif /* STRATUM_BOOT_ELF_H */
