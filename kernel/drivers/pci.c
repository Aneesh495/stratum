#include <kernel/pci.h>
#include <kernel/x86_64.h>
#include <kernel/kernel.h>
#include <kernel/string.h>
#include <kernel/pmm.h>
#include <kernel/vmm.h>
#include <kernel/slab.h>

static pci_device_t g_pci_devices[PCI_MAX_DEVICES];
static uint32_t     g_pci_device_count = 0;
static pci_device_t *g_pci_device_list = NULL;

static inline uint32_t pci_make_address(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    return (1U << 31) |
           ((uint32_t)bus << 16) |
           ((uint32_t)slot << 11) |
           ((uint32_t)func << 8) |
           ((uint32_t)offset & 0xFC);
}

uint8_t pci_read_config8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t address = pci_make_address(bus, slot, func, offset);
    outl(PCI_CONFIG_ADDRESS, address);
    return inb((uint16_t)(PCI_CONFIG_DATA + (offset & 3)));
}

uint16_t pci_read_config16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t address = pci_make_address(bus, slot, func, offset);
    outl(PCI_CONFIG_ADDRESS, address);
    return inw((uint16_t)(PCI_CONFIG_DATA + (offset & 2)));
}

uint32_t pci_read_config32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t address = pci_make_address(bus, slot, func, offset);
    outl(PCI_CONFIG_ADDRESS, address);
    return inl(PCI_CONFIG_DATA);
}

void pci_write_config8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint8_t val) {
    uint32_t address = pci_make_address(bus, slot, func, offset);
    outl(PCI_CONFIG_ADDRESS, address);
    outb((uint16_t)(PCI_CONFIG_DATA + (offset & 3)), val);
}

void pci_write_config16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint16_t val) {
    uint32_t address = pci_make_address(bus, slot, func, offset);
    outl(PCI_CONFIG_ADDRESS, address);
    outw((uint16_t)(PCI_CONFIG_DATA + (offset & 2)), val);
}

void pci_write_config32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t val) {
    uint32_t address = pci_make_address(bus, slot, func, offset);
    outl(PCI_CONFIG_ADDRESS, address);
    outl(PCI_CONFIG_DATA, val);
}

static void probe_bars(pci_device_t *dev) {
    dev->num_bars = 6;
    for (int bar_idx = 0; bar_idx < 6; bar_idx++) {
        uint8_t offset = (uint8_t)(PCI_REG_BAR0 + bar_idx * 4);
        uint32_t orig = pci_read_config32(dev->bus, dev->slot, dev->func, offset);

        /* Write 0xFFFFFFFF to probe size */
        pci_write_config32(dev->bus, dev->slot, dev->func, offset, 0xFFFFFFFF);
        uint32_t mask = pci_read_config32(dev->bus, dev->slot, dev->func, offset);
        pci_write_config32(dev->bus, dev->slot, dev->func, offset, orig);

        if (mask == 0 || mask == 0xFFFFFFFF) {
            dev->bars[bar_idx].base_phys = 0;
            dev->bars[bar_idx].size = 0;
            continue;
        }

        if (orig & 1) {
            /* I/O Space BAR */
            dev->bars[bar_idx].is_io = true;
            dev->bars[bar_idx].is_64bit = false;
            dev->bars[bar_idx].is_prefetchable = false;
            dev->bars[bar_idx].base_phys = orig & ~0x3ULL;
            dev->bars[bar_idx].size = (~(mask & ~0x3ULL) + 1) & 0xFFFF;
            dev->bars[bar_idx].base_virt = (void *)(uintptr_t)dev->bars[bar_idx].base_phys;
        } else {
            /* Memory Space BAR */
            dev->bars[bar_idx].is_io = false;
            uint8_t type = (orig >> 1) & 3;
            dev->bars[bar_idx].is_64bit = (type == 2);
            dev->bars[bar_idx].is_prefetchable = (orig & (1 << 3)) != 0;

            if (dev->bars[bar_idx].is_64bit && bar_idx < 5) {
                uint8_t next_offset = (uint8_t)(offset + 4);
                uint32_t orig_hi = pci_read_config32(dev->bus, dev->slot, dev->func, next_offset);
                pci_write_config32(dev->bus, dev->slot, dev->func, next_offset, 0xFFFFFFFF);
                uint32_t mask_hi = pci_read_config32(dev->bus, dev->slot, dev->func, next_offset);
                pci_write_config32(dev->bus, dev->slot, dev->func, next_offset, orig_hi);

                uint64_t full_base = ((uint64_t)orig_hi << 32) | (orig & ~0xFULL);
                uint64_t full_mask = ((uint64_t)mask_hi << 32) | (mask & ~0xFULL);
                uint64_t full_size;
                if (mask_hi == 0) {
                    full_size = (~(mask & ~0xFULL) + 1) & 0xFFFFFFFFULL;
                } else {
                    full_size = ~(full_mask) + 1;
                }

                dev->bars[bar_idx].base_phys = full_base;
                dev->bars[bar_idx].size = full_size;
                dev->bars[bar_idx].base_virt = phys_to_virt(full_base);

                /* Map MMIO pages into kernel address space if outside the 4 GiB HHDM window */
                if (full_base >= 0x100000000ULL && full_size != 0) {
                    uint64_t pages = (full_size + PAGE_SIZE - 1) / PAGE_SIZE;
                    for (uint64_t p = 0; p < pages; p++) {
                        vmm_map_page(g_kernel_pml4, (uint64_t)phys_to_virt(full_base + p * PAGE_SIZE),
                                     full_base + p * PAGE_SIZE, PTE_PRESENT | PTE_WRITABLE | PTE_PCD);
                    }
                }

                bar_idx++; /* Skip high 32-bit BAR */
            } else {
                dev->bars[bar_idx].base_phys = orig & ~0xFULL;
                dev->bars[bar_idx].size = (~(mask & ~0xFULL) + 1) & 0xFFFFFFFF;
                dev->bars[bar_idx].base_virt = phys_to_virt(dev->bars[bar_idx].base_phys);

                /* 32-bit BARs are always < 4 GiB and thus already mapped via HHDM */
                if (dev->bars[bar_idx].base_phys >= 0x100000000ULL && dev->bars[bar_idx].size != 0) {
                    uint64_t pages = (dev->bars[bar_idx].size + PAGE_SIZE - 1) / PAGE_SIZE;
                    for (uint64_t p = 0; p < pages; p++) {
                        vmm_map_page(g_kernel_pml4,
                                     (uint64_t)phys_to_virt(dev->bars[bar_idx].base_phys + p * PAGE_SIZE),
                                     dev->bars[bar_idx].base_phys + p * PAGE_SIZE,
                                     PTE_PRESENT | PTE_WRITABLE | PTE_PCD);
                    }
                }
            }
        }
    }
}

static void probe_capabilities(pci_device_t *dev) {
    uint16_t status = pci_read_config16(dev->bus, dev->slot, dev->func, PCI_REG_STATUS);
    if (!(status & PCI_STATUS_CAPABILITIES)) {
        return;
    }

    uint8_t cap_ptr = pci_read_config8(dev->bus, dev->slot, dev->func, PCI_REG_CAPABILITIES_PTR) & ~0x3;
    pci_cap_t **curr = &dev->caps;

    while (cap_ptr != 0 && cap_ptr >= 0x40) {
        uint8_t cap_id = pci_read_config8(dev->bus, dev->slot, dev->func, cap_ptr);
        uint8_t next_ptr = pci_read_config8(dev->bus, dev->slot, dev->func, (uint8_t)(cap_ptr + 1)) & ~0x3;

        pci_cap_t *cap = (pci_cap_t *)kmalloc(sizeof(pci_cap_t));
        if (!cap) break;

        cap->id = cap_id;
        cap->offset = cap_ptr;
        cap->length = pci_read_config8(dev->bus, dev->slot, dev->func, (uint8_t)(cap_ptr + 2));
        cap->next = NULL;

        *curr = cap;
        curr = &cap->next;

        cap_ptr = next_ptr;
    }
}

static void probe_function(uint8_t bus, uint8_t slot, uint8_t func) {
    uint16_t vendor_id = pci_read_config16(bus, slot, func, PCI_REG_VENDOR_ID);
    if (vendor_id == 0xFFFF) return;

    if (g_pci_device_count >= PCI_MAX_DEVICES) return;

    pci_device_t *dev = &g_pci_devices[g_pci_device_count++];
    memset(dev, 0, sizeof(pci_device_t));

    dev->bus = bus;
    dev->slot = slot;
    dev->func = func;
    dev->vendor_id = vendor_id;
    dev->device_id = pci_read_config16(bus, slot, func, PCI_REG_DEVICE_ID);
    dev->revision_id = pci_read_config8(bus, slot, func, PCI_REG_REVISION_ID);
    dev->prog_if = pci_read_config8(bus, slot, func, PCI_REG_PROG_IF);
    dev->subclass = pci_read_config8(bus, slot, func, PCI_REG_SUBCLASS);
    dev->class_code = pci_read_config8(bus, slot, func, PCI_REG_CLASS);
    dev->header_type = pci_read_config8(bus, slot, func, PCI_REG_HEADER_TYPE);
    dev->irq_line = pci_read_config8(bus, slot, func, PCI_REG_INTERRUPT_LINE);
    dev->irq_pin = pci_read_config8(bus, slot, func, PCI_REG_INTERRUPT_PIN);

    probe_bars(dev);
    probe_capabilities(dev);

    /* Link into global device list */
    dev->next = g_pci_device_list;
    g_pci_device_list = dev;

    kprintf("[PCI] %02x:%02x.%x Vendor:%04x Device:%04x Class:%02x Sub:%02x (IRQ line=%u pin=%u)\n",
            bus, slot, func, dev->vendor_id, dev->device_id, dev->class_code, dev->subclass,
            dev->irq_line, dev->irq_pin);
}

void pci_init(void) {
    g_pci_device_count = 0;
    g_pci_device_list = NULL;

    kprintf("[PCI] Scanning PCI bus hierarchy...\n");

    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            uint16_t vendor_id = pci_read_config16((uint8_t)bus, slot, 0, PCI_REG_VENDOR_ID);
            if (vendor_id == 0xFFFF) continue;

            probe_function((uint8_t)bus, slot, 0);

            uint8_t header_type = pci_read_config8((uint8_t)bus, slot, 0, PCI_REG_HEADER_TYPE);
            if (header_type & 0x80) {
                /* Multi-function device: scan functions 1..7 */
                for (uint8_t func = 1; func < 8; func++) {
                    uint16_t fn_vendor = pci_read_config16((uint8_t)bus, slot, func, PCI_REG_VENDOR_ID);
                    if (fn_vendor != 0xFFFF) {
                        probe_function((uint8_t)bus, slot, func);
                    }
                }
            }
        }
    }

    kprintf("[PCI] Enumeration complete: %u PCI devices registered.\n", g_pci_device_count);
}

pci_device_t *pci_find_device(uint16_t vendor_id, uint16_t device_id) {
    for (pci_device_t *d = g_pci_device_list; d != NULL; d = d->next) {
        if (d->vendor_id == vendor_id && d->device_id == device_id) {
            return d;
        }
    }
    return NULL;
}

pci_device_t *pci_find_class(uint8_t class_code, uint8_t subclass) {
    for (pci_device_t *d = g_pci_device_list; d != NULL; d = d->next) {
        if (d->class_code == class_code && d->subclass == subclass) {
            return d;
        }
    }
    return NULL;
}

void pci_enable_bus_mastering(pci_device_t *dev) {
    if (!dev) return;
    uint16_t cmd = pci_read_config16(dev->bus, dev->slot, dev->func, PCI_REG_COMMAND);
    cmd |= (PCI_CMD_BUS_MASTER | PCI_CMD_MEMORY_SPACE | PCI_CMD_IO_SPACE);
    pci_write_config16(dev->bus, dev->slot, dev->func, PCI_REG_COMMAND, cmd);
}

uint8_t pci_find_capability(pci_device_t *dev, uint8_t cap_id) {
    if (!dev) return 0;
    for (pci_cap_t *cap = dev->caps; cap != NULL; cap = cap->next) {
        if (cap->id == cap_id) {
            return cap->offset;
        }
    }
    return 0;
}

pci_device_t *pci_get_device_list(void) {
    return g_pci_device_list;
}

#define MSIX_ENABLE       0x8000U
#define MSIX_FUNCTION_MASK 0x4000U

typedef struct {
    uint32_t address_low;
    uint32_t address_high;
    uint32_t data;
    uint32_t control;
} msix_entry_t;

bool pci_configure_msix(pci_device_t *dev, uint16_t entry, uint8_t vector, uint8_t apic_id) {
    uint8_t cap = pci_find_capability(dev, PCI_CAP_ID_MSIX);
    if (!cap || cap > 244 || vector < 32) return false;
    uint16_t control = pci_read_config16(dev->bus, dev->slot, dev->func, cap + 2);
    unsigned count = (control & 0x7ffU) + 1;
    uint32_t location = pci_read_config32(dev->bus, dev->slot, dev->func, cap + 4);
    unsigned bar_index = location & 7;
    uint32_t offset = location & ~7U;
    if (entry >= count || bar_index >= 6) return false;

    const pci_bar_t *bar = &dev->bars[bar_index];
    if (bar->is_io || !bar->base_virt || offset > bar->size ||
        count * sizeof(msix_entry_t) > bar->size - offset) return false;
    volatile msix_entry_t *table = (volatile void *)((uint8_t *)bar->base_virt + offset);
    pci_write_config16(dev->bus, dev->slot, dev->func, cap + 2, control | MSIX_ENABLE | MSIX_FUNCTION_MASK);
    for (unsigned i = 0; i < count; i++) table[i].control = 1;
    table[entry].address_low = 0xfee00000U | ((uint32_t)apic_id << 12);
    table[entry].address_high = 0;
    table[entry].data = vector;
    table[entry].control = 0;
    (void)table[entry].control;
    pci_write_config16(dev->bus, dev->slot, dev->func, cap + 2, (control | MSIX_ENABLE) & ~MSIX_FUNCTION_MASK);
    return true;
}

void pci_disable_msix(pci_device_t *dev) {
    uint8_t cap = pci_find_capability(dev, PCI_CAP_ID_MSIX);
    if (!cap || cap > 244) return;
    uint16_t control = pci_read_config16(dev->bus, dev->slot, dev->func, cap + 2);
    pci_write_config16(dev->bus, dev->slot, dev->func, cap + 2, (control | MSIX_FUNCTION_MASK) & ~MSIX_ENABLE);
}
