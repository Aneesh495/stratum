#ifndef STRATUM_KERNEL_ACPI_H
#define STRATUM_KERNEL_ACPI_H

#include <kernel/types.h>
#include <shared/boot_info.h>

#define ACPI_MAX_CPUS    32
#define ACPI_MAX_IOAPICS 4
#define ACPI_MAX_ISOS    32

typedef struct {
    char     signature[8];
    uint8_t  checksum;
    char     oem_id[6];
    uint8_t  revision;
    uint32_t rsdt_addr;
    uint32_t length;
    uint64_t xsdt_addr;
    uint8_t  extended_checksum;
    uint8_t  reserved[3];
} __attribute__((packed)) acpi_rsdp_t;

typedef struct {
    char     signature[4];
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oem_id[6];
    char     oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} __attribute__((packed)) acpi_sdt_header_t;

typedef struct {
    acpi_sdt_header_t header;
    uint32_t          lapic_addr;
    uint32_t          flags;
} __attribute__((packed)) acpi_madt_t;

#define MADT_TYPE_LAPIC               0
#define MADT_TYPE_IOAPIC              1
#define MADT_TYPE_ISO                 2
#define MADT_TYPE_NMI                 4
#define MADT_TYPE_LAPIC_ADDR_OVERRIDE 5

typedef struct {
    uint8_t type;
    uint8_t length;
} __attribute__((packed)) madt_entry_header_t;

typedef struct {
    madt_entry_header_t header;
    uint8_t  acpi_processor_id;
    uint8_t  apic_id;
    uint32_t flags;
} __attribute__((packed)) madt_lapic_t;

typedef struct {
    madt_entry_header_t header;
    uint8_t  ioapic_id;
    uint8_t  reserved;
    uint32_t ioapic_addr;
    uint32_t gsi_base;
} __attribute__((packed)) madt_ioapic_t;

typedef struct {
    madt_entry_header_t header;
    uint8_t  bus;
    uint8_t  source;
    uint32_t gsi;
    uint16_t flags;
} __attribute__((packed)) madt_iso_t;

typedef struct {
    uint64_t lapic_phys;
    uint32_t cpu_count;
    uint8_t  cpu_apic_ids[ACPI_MAX_CPUS];
    bool     cpu_enabled[ACPI_MAX_CPUS];

    uint32_t ioapic_count;
    uint64_t ioapic_phys[ACPI_MAX_IOAPICS];
    uint32_t ioapic_gsi_base[ACPI_MAX_IOAPICS];

    uint32_t   iso_count;
    madt_iso_t isos[ACPI_MAX_ISOS];
} acpi_info_t;

extern acpi_info_t g_acpi_info;

int  acpi_init(const boot_handoff_t *handoff);
void *acpi_find_table(const char *signature);

int acpi_parse_madt(const void *buffer, size_t length, acpi_info_t *out);
#endif /* STRATUM_KERNEL_ACPI_H */
