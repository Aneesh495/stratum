#include <kernel/acpi.h>
#include <kernel/pmm.h>
#include <kernel/kernel.h>
#include <kernel/string.h>

acpi_info_t g_acpi_info;

static acpi_rsdp_t       *g_rsdp = NULL;
static acpi_sdt_header_t *g_rsdt = NULL;
static acpi_sdt_header_t *g_xsdt = NULL;

static bool acpi_checksum(const void *ptr, uint32_t len) {
    const uint8_t *bytes = (const uint8_t *)ptr;
    uint8_t sum = 0;
    for (uint32_t i = 0; i < len; i++) {
        sum += bytes[i];
    }
    return sum == 0;
}

static acpi_sdt_header_t *get_sdt(uint64_t phys) {
    if (!phys || phys > STRATUM_BOOT_PHYS_LIMIT - sizeof(acpi_sdt_header_t)) return NULL;
    acpi_sdt_header_t *header = phys_to_virt(phys);
    if (header->length < sizeof(*header) || header->length > 1024 * 1024 ||
        header->length > STRATUM_BOOT_PHYS_LIMIT - phys ||
        !acpi_checksum(header, header->length)) return NULL;
    return header;
}

void *acpi_find_table(const char *sig) {
    if (!sig) return NULL;

    if (g_xsdt) {
        uint32_t entries = (g_xsdt->length - sizeof(acpi_sdt_header_t)) / 8;
        const uint8_t *table_ptrs = (const uint8_t *)g_xsdt + sizeof(acpi_sdt_header_t);

        for (uint32_t i = 0; i < entries; i++) {
            uint64_t phys;
            memcpy(&phys, table_ptrs + i * sizeof(phys), sizeof(phys));
            if (phys == 0) continue;
            acpi_sdt_header_t *hdr = get_sdt(phys);
            if (hdr && memcmp(hdr->signature, sig, 4) == 0) return hdr;
        }
    } else if (g_rsdt) {
        uint32_t entries = (g_rsdt->length - sizeof(acpi_sdt_header_t)) / 4;
        uint32_t *table_ptrs = (uint32_t *)((uint8_t *)g_rsdt + sizeof(acpi_sdt_header_t));

        for (uint32_t i = 0; i < entries; i++) {
            uint32_t phys = table_ptrs[i];
            if (phys == 0) continue;
            acpi_sdt_header_t *hdr = get_sdt(phys);
            if (hdr && memcmp(hdr->signature, sig, 4) == 0) return hdr;
        }
    }

    return NULL;
}

int acpi_init(const boot_handoff_t *handoff) {
    memset(&g_acpi_info, 0, sizeof(g_acpi_info));

    if (!handoff || handoff->rsdp_phys == 0) {
        kprintf("[ACPI] WARNING: No RSDP physical pointer provided.\n");
        return -1;
    }

    g_rsdp = (acpi_rsdp_t *)phys_to_virt(handoff->rsdp_phys);
    if (memcmp(g_rsdp->signature, "RSD PTR ", 8) != 0) {
        kprintf("[ACPI] ERROR: Invalid RSDP signature.\n");
        return -2;
    }

    if (!acpi_checksum(g_rsdp, 20)) {
        kprintf("[ACPI] ERROR: RSDP checksum mismatch.\n");
        return -3;
    }

    kprintf("[ACPI] RSDP found (revision %u)\n", (unsigned int)g_rsdp->revision);

    if (g_rsdp->revision >= 2 && g_rsdp->length >= sizeof(*g_rsdp) &&
        g_rsdp->length <= 4096 && g_rsdp->length <= STRATUM_BOOT_PHYS_LIMIT - handoff->rsdp_phys &&
        acpi_checksum(g_rsdp, g_rsdp->length) && g_rsdp->xsdt_addr != 0) {
        g_xsdt = get_sdt(g_rsdp->xsdt_addr);
        if (!g_xsdt || memcmp(g_xsdt->signature, "XSDT", 4)) {
            kprintf("[ACPI] WARNING: XSDT checksum invalid, falling back to RSDT.\n");
            g_xsdt = NULL;
        } else {
            kprintf("[ACPI] Using XSDT at 0x%lx\n", g_rsdp->xsdt_addr);
        }
    }

    if (!g_xsdt && g_rsdp->rsdt_addr != 0) {
        g_rsdt = get_sdt(g_rsdp->rsdt_addr);
        if (!g_rsdt || memcmp(g_rsdt->signature, "RSDT", 4)) {
            kprintf("[ACPI] ERROR: RSDT checksum invalid.\n");
            return -4;
        }
        kprintf("[ACPI] Using RSDT at 0x%x\n", g_rsdp->rsdt_addr);
    }

    acpi_madt_t *madt = (acpi_madt_t *)acpi_find_table("APIC");
    if (!madt) {
        kprintf("[ACPI] ERROR: MADT (APIC) table not found.\n");
        return -5;
    }

    kprintf("[ACPI] MADT table found at 0x%p (length: %u bytes)\n", madt, madt->header.length);
    if (acpi_parse_madt(madt, madt->header.length, &g_acpi_info) != 0) {
        memset(&g_acpi_info, 0, sizeof(g_acpi_info));
        return -6;
    }

    kprintf("[ACPI] MADT parsed: LAPIC Base: 0x%lx, CPUs found: %u, I/O APICs: %u, ISOs: %u\n",
            g_acpi_info.lapic_phys, g_acpi_info.cpu_count,
            g_acpi_info.ioapic_count, g_acpi_info.iso_count);

    for (uint32_t i = 0; i < g_acpi_info.cpu_count; i++) {
        kprintf("[ACPI]   CPU #%u: APIC ID %u (enabled=%d)\n",
                i, g_acpi_info.cpu_apic_ids[i], g_acpi_info.cpu_enabled[i]);
    }

    return 0;
}
