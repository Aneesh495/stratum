#include <kernel/acpi.h>
#include <kernel/pmm.h>
#include <kernel/string.h>

int acpi_parse_madt(const void *buffer, size_t length, acpi_info_t *out) {
    if (length < sizeof(acpi_madt_t)) return -1;
    const acpi_madt_t *madt = buffer;
    if (madt->header.length < sizeof(*madt) || madt->header.length > length) return -1;

    out->lapic_phys = madt->lapic_addr;
    const uint8_t *cur = (const uint8_t *)madt + sizeof(acpi_madt_t);
    const uint8_t *end = (const uint8_t *)madt + madt->header.length;

    while (cur < end) {
        if ((size_t)(end - cur) < sizeof(madt_entry_header_t)) return -1;
        const madt_entry_header_t *entry = (const madt_entry_header_t *)cur;
        if (entry->length < sizeof(*entry) || entry->length > end - cur) return -1;

        switch (entry->type) {
            case MADT_TYPE_LAPIC: {
                if (entry->length < sizeof(madt_lapic_t)) return -1;
                const madt_lapic_t *lapic = (const madt_lapic_t *)cur;
                if (out->cpu_count < ACPI_MAX_CPUS) {
                    uint32_t idx = out->cpu_count++;
                    out->cpu_apic_ids[idx] = lapic->apic_id;
                    out->cpu_enabled[idx] = (lapic->flags & 0x03) != 0;
                }
                break;
            }
            case MADT_TYPE_IOAPIC: {
                if (entry->length < sizeof(madt_ioapic_t)) return -1;
                const madt_ioapic_t *ioapic = (const madt_ioapic_t *)cur;
                if (out->ioapic_count < ACPI_MAX_IOAPICS) {
                    uint32_t idx = out->ioapic_count++;
                    out->ioapic_phys[idx] = ioapic->ioapic_addr;
                    out->ioapic_gsi_base[idx] = ioapic->gsi_base;
                }
                break;
            }
            case MADT_TYPE_ISO: {
                if (entry->length < sizeof(madt_iso_t)) return -1;
                const madt_iso_t *iso = (const madt_iso_t *)cur;
                if (out->iso_count < ACPI_MAX_ISOS) {
                    out->isos[out->iso_count++] = *iso;
                }
                break;
            }
            case MADT_TYPE_LAPIC_ADDR_OVERRIDE: {
                if (entry->length < 12) return -1;
                memcpy(&out->lapic_phys, cur + 4, sizeof(out->lapic_phys));
                break;
            }
            default:
                break;
        }

        cur += entry->length;
    }
    if (out->lapic_phys > STRATUM_BOOT_PHYS_LIMIT - PAGE_SIZE) return -1;
    return 0;
}
