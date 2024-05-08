#include <stddef.h>
#include <stdint.h>
#include <string.h>

extern void abort(void);

#define VERIFY(condition) do { if (!(condition)) abort(); } while (0)

#if defined(FUZZ_ELF)

#include "elf.h"
#include <shared/boot_info.h>
#include <stdlib.h>

typedef struct {
    EFI_FILE_PROTOCOL file;
    const uint8_t *bytes;
    uint64_t size, offset;
} input_file_t;

static EFI_STATUS input_seek(EFI_FILE_PROTOCOL *file, uint64_t offset) {
    input_file_t *input = (input_file_t *)file;
    if (offset > input->size) return EFI_LOAD_ERROR;
    input->offset = offset;
    return EFI_SUCCESS;
}

static EFI_STATUS input_read(EFI_FILE_PROTOCOL *file, uint64_t *length, void *buffer) {
    input_file_t *input = (input_file_t *)file;
    if (*length > input->size - input->offset) *length = input->size - input->offset;
    memcpy(buffer, input->bytes + input->offset, (size_t)*length);
    input->offset += *length;
    return EFI_SUCCESS;
}

static EFI_STATUS alloc_pool(EFI_MEMORY_TYPE type, uint64_t size, void **out) {
    (void)type;
    *out = malloc((size_t)size);
    return *out ? EFI_SUCCESS : EFI_OUT_OF_RESOURCES;
}

static EFI_STATUS free_pool(void *memory) {
    free(memory);
    return EFI_SUCCESS;
}

static EFI_STATUS alloc_pages(EFI_ALLOCATE_TYPE type, EFI_MEMORY_TYPE kind, uint64_t pages, uint64_t *out) {
    (void)type;
    (void)kind;
    if (!pages || pages > 64) return EFI_OUT_OF_RESOURCES;
    void *memory = malloc((size_t)pages * 4096);
    if (!memory) return EFI_OUT_OF_RESOURCES;
    *out = (uintptr_t)memory;
    return EFI_SUCCESS;
}

static EFI_STATUS free_pages(uint64_t address, uint64_t pages) {
    (void)pages;
    free((void *)(uintptr_t)address);
    return EFI_SUCCESS;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < sizeof(Elf64_Ehdr)) return 0;
    Elf64_Ehdr header;
    memcpy(&header, data, sizeof(header));
    if (elf_validate_header(&header, size)) return 0;

    EFI_BOOT_SERVICES services = {
        .AllocatePool = alloc_pool, .FreePool = free_pool,
        .AllocatePages = alloc_pages, .FreePages = free_pages,
    };
    input_file_t input = {
        .file = { .SetPosition = input_seek, .Read = input_read },
        .bytes = data, .size = size,
    };
    elf_loaded_image_t image;
    if (elf_load_segments(&services, &input.file, &header, size, &image) == EFI_SUCCESS) {
        VERIFY(image.virt_base == STRATUM_KERNEL_BASE && image.entry_point == header.e_entry);
        VERIFY(image.phys_size <= 64 * 4096 && image.phys_size % 4096 == 0);
        VERIFY(image.entry_point >= image.virt_base && image.entry_point - image.virt_base < image.phys_size);
        free_pages(image.phys_base, image.phys_size / 4096);
    }
    return 0;
}

#elif defined(FUZZ_HANDOFF)

#include <kernel/boot.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < sizeof(boot_handoff_t)) return 0;
    boot_handoff_t handoff;
    memcpy(&handoff, data, sizeof(handoff));
    if (boot_check_header(&handoff, 0x8000, STRATUM_BOOT_MAGIC)) return 0;
    VERIFY(handoff.mem_map_entries && handoff.mem_map_entries <= STRATUM_BOOT_MAX_DESCS);
    size_t bytes = handoff.mem_map_entries * sizeof(boot_mem_desc_t);
    if (bytes > size - sizeof(handoff)) return 0;
    boot_mem_desc_t entries[STRATUM_BOOT_MAX_DESCS];
    memcpy(entries, data + sizeof(handoff), bytes);
    if (!boot_check_descriptors(&handoff, 0x8000, entries)) {
        for (unsigned i = 0; i < handoff.mem_map_entries; i++) {
            VERIFY(entries[i].page_count && !(entries[i].phys_addr & 4095));
        }
    }
    return 0;
}

#elif defined(FUZZ_CPIO)

#include <kernel/cpio.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    cpio_file_t files[64];
    unsigned count;
    if (cpio_scan_bin(data, size, files, 64, &count)) return 0;
    VERIFY(count && count <= 64);
    for (unsigned i = 0; i < count; i++) {
        VERIFY(files[i].data >= data && files[i].data <= data + size);
        VERIFY(files[i].size <= (size_t)(data + size - files[i].data));
        VERIFY(files[i].name >= (const char *)data && files[i].name < (const char *)data + size);
        const char *end = memchr(files[i].name, 0, (const char *)data + size - files[i].name);
        VERIFY(end && end - files[i].name < 64);
    }
    return 0;
}

#elif defined(FUZZ_ACPI)

#include <kernel/acpi.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    acpi_info_t parsed = {0};
    if (acpi_parse_madt(data, size, &parsed)) return 0;
    VERIFY(parsed.cpu_count <= ACPI_MAX_CPUS && parsed.ioapic_count <= ACPI_MAX_IOAPICS &&
           parsed.iso_count <= ACPI_MAX_ISOS);
    VERIFY(parsed.lapic_phys < STRATUM_BOOT_PHYS_LIMIT);
    return 0;
}

#elif defined(FUZZ_STRATAFS)

#include <kernel/stratafs_dirent.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    for (size_t i = 0; i + sizeof(stratafs_disk_dirent_t) <= size; i += sizeof(stratafs_disk_dirent_t)) {
        const stratafs_disk_dirent_t *entry = (const void *)(data + i);
        if (!stratafs_valid_dirent(entry, 2048) || !entry->inode_num) continue;
        VERIFY(entry->inode_num < 2048 && entry->name_len > 0 && entry->name_len < VFS_MAX_NAME);
        VERIFY(!memchr(entry->name, '\0', entry->name_len) && !memchr(entry->name, '/', entry->name_len));
    }
    return 0;
}

#elif defined(FUZZ_IPV4)

#include <kernel/ipv4.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    ipv4_view_t view;
    if (!size || ipv4_decode(data + 1, size - 1, &view)) return 0;
    VERIFY(view.header_size >= sizeof(ip_hdr_t) && view.header_size <= 60);
    VERIFY(view.header_size + view.payload_size <= size - 1);
    return 0;
}

#endif
