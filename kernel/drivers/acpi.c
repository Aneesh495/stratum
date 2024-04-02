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

void *acpi_find_table(const char *sig) {
    if (!sig) return NULL;

    if (g_xsdt) {
        uint32_t entries = (g_xsdt->length - sizeof(acpi_sdt_header_t)) / 8;
        uint64_t *table_ptrs = (uint64_t *)((uint8_t *)g_xsdt + sizeof(acpi_sdt_header_t));

        for (uint32_t i = 0; i < entries; i++) {
            uint64_t phys = table_ptrs[i];
            if (phys == 0) continue;
            acpi_sdt_header_t *hdr = (acpi_sdt_header_t *)phys_to_virt(phys);
            if (memcmp(hdr->signature, sig, 4) == 0) {
                if (acpi_checksum(hdr, hdr->length)) {
                    return hdr;
                }
            }
        }
    } else if (g_rsdt) {
        uint32_t entries = (g_rsdt->length - sizeof(acpi_sdt_header_t)) / 4;
        uint32_t *table_ptrs = (uint32_t *)((uint8_t *)g_rsdt + sizeof(acpi_sdt_header_t));

        for (uint32_t i = 0; i < entries; i++) {
            uint32_t phys = table_ptrs[i];
            if (phys == 0) continue;
            acpi_sdt_header_t *hdr = (acpi_sdt_header_t *)phys_to_virt(phys);
            if (memcmp(hdr->signature, sig, 4) == 0) {
                if (acpi_checksum(hdr, hdr->length)) {
                    return hdr;
                }
            }
        }
    }

    return NULL;
}

static void acpi_parse_madt(acpi_madt_t *madt) {
    if (!madt) return;

    g_acpi_info.lapic_phys = madt->lapic_addr;
    uint8_t *cur = (uint8_t *)madt + sizeof(acpi_madt_t);
    uint8_t *end = (uint8_t *)madt + madt->header.length;

    while (cur < end) {
        madt_entry_header_t *entry = (madt_entry_header_t *)cur;
        if (entry->length == 0) break; /* Avoid infinite loop on malformed entry */

        switch (entry->type) {
            case MADT_TYPE_LAPIC: {
                madt_lapic_t *lapic = (madt_lapic_t *)cur;
                if (g_acpi_info.cpu_count < ACPI_MAX_CPUS) {
                    uint32_t idx = g_acpi_info.cpu_count++;
                    g_acpi_info.cpu_apic_ids[idx] = lapic->apic_id;
                    g_acpi_info.cpu_enabled[idx] = (lapic->flags & 0x03) != 0;
                }
                break;
            }
            case MADT_TYPE_IOAPIC: {
                madt_ioapic_t *ioapic = (madt_ioapic_t *)cur;
                if (g_acpi_info.ioapic_count < ACPI_MAX_IOAPICS) {
                    uint32_t idx = g_acpi_info.ioapic_count++;
                    g_acpi_info.ioapic_phys[idx] = ioapic->ioapic_addr;
                    g_acpi_info.ioapic_gsi_base[idx] = ioapic->gsi_base;
                }
                break;
            }
            case MADT_TYPE_ISO: {
                madt_iso_t *iso = (madt_iso_t *)cur;
                if (g_acpi_info.iso_count < ACPI_MAX_ISOS) {
                    g_acpi_info.isos[g_acpi_info.iso_count++] = *iso;
                }
                break;
            }
            case MADT_TYPE_LAPIC_ADDR_OVERRIDE: {
                uint64_t *override_addr = (uint64_t *)(cur + 4);
                g_acpi_info.lapic_phys = *override_addr;
                break;
            }
            default:
                break;
        }

        cur += entry->length;
    }
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

    if (g_rsdp->revision >= 2 && g_rsdp->xsdt_addr != 0) {
        g_xsdt = (acpi_sdt_header_t *)phys_to_virt(g_rsdp->xsdt_addr);
        if (!acpi_checksum(g_xsdt, g_xsdt->length)) {
            kprintf("[ACPI] WARNING: XSDT checksum invalid, falling back to RSDT.\n");
            g_xsdt = NULL;
        } else {
            kprintf("[ACPI] Using XSDT at 0x%lx\n", g_rsdp->xsdt_addr);
        }
    }

    if (!g_xsdt && g_rsdp->rsdt_addr != 0) {
        g_rsdt = (acpi_sdt_header_t *)phys_to_virt(g_rsdp->rsdt_addr);
        if (!acpi_checksum(g_rsdt, g_rsdt->length)) {
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
    acpi_parse_madt(madt);

    kprintf("[ACPI] MADT parsed: LAPIC Base: 0x%lx, CPUs found: %u, I/O APICs: %u, ISOs: %u\n",
            g_acpi_info.lapic_phys, g_acpi_info.cpu_count,
            g_acpi_info.ioapic_count, g_acpi_info.iso_count);

    for (uint32_t i = 0; i < g_acpi_info.cpu_count; i++) {
        kprintf("[ACPI]   CPU #%u: APIC ID %u (enabled=%d)\n",
                i, g_acpi_info.cpu_apic_ids[i], g_acpi_info.cpu_enabled[i]);
    }

    return 0;
}
