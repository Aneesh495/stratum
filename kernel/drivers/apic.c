#include <kernel/apic.h>
#include <kernel/pmm.h>
#include <kernel/kernel.h>
#include <kernel/x86_64.h>

static volatile uint32_t *g_lapic_regs = NULL;
static volatile uint32_t *g_ioapic_regs = NULL;

static inline uint32_t lapic_read(uint32_t reg) {
    return g_lapic_regs[reg / 4];
}

static inline void lapic_write(uint32_t reg, uint32_t val) {
    g_lapic_regs[reg / 4] = val;
}

static inline uint32_t ioapic_read(uint32_t reg) {
    g_ioapic_regs[0] = reg;
    return g_ioapic_regs[4];
}

static inline void ioapic_write(uint32_t reg, uint32_t val) {
    g_ioapic_regs[0] = reg;
    g_ioapic_regs[4] = val;
}

/* PIT-based delay helper (milliseconds) */
void pit_delay_ms(uint32_t ms) {
    /* Configure PIT Channel 2 (one-shot countdown) */
    for (uint32_t i = 0; i < ms; i++) {
        /* Set channel 2 frequency divider for 1 ms (1193182 / 1000 = 1193) */
        uint8_t gate = inb(0x61);
        outb(0x61, (gate & ~0x02) | 0x01); /* Disable gate, clear output */
        outb(0x43, 0xB0);                  /* Channel 2, mode 0, binary */
        outb(0x42, 1193 & 0xFF);
        outb(0x42, (1193 >> 8) & 0xFF);

        /* Reset and enable gate */
        gate = inb(0x61);
        outb(0x61, (gate & ~0x01) | 0x01);

        /* Wait until OUT2 goes high */
        while ((inb(0x61) & 0x20) == 0) {
            cpu_pause();
        }
    }
}

void lapic_init(void) {
    if (!g_lapic_regs) {
        uint64_t lapic_phys = g_acpi_info.lapic_phys ? g_acpi_info.lapic_phys : 0xFEE00000ULL;
        g_lapic_regs = (volatile uint32_t *)phys_to_virt(lapic_phys);
    }

    /* 1. Ensure Local APIC is globally enabled in MSR */
    uint64_t apic_msr = rdmsr(MSR_APIC_BASE);
    if (!(apic_msr & (1ULL << 11))) {
        wrmsr(MSR_APIC_BASE, apic_msr | (1ULL << 11));
    }

    /* 2. Configure Flat Destination Mode */
    lapic_write(LAPIC_DFR, 0xFFFFFFFF);

    /* 3. Configure Logical Destination ID */
    uint32_t ldr = (lapic_read(LAPIC_ID) & 0xFF000000);
    lapic_write(LAPIC_LDR, ldr);

    /* 4. Enable Software APIC and set Spurious Interrupt Vector */
    lapic_write(LAPIC_SVR, 0x100 | VEC_SPURIOUS);

    /* 5. Set Task Priority to accept all interrupts */
    lapic_write(LAPIC_TPR, 0);

    /* 6. Mask extraneous LVT entries */
    lapic_write(LAPIC_LVT_LINT0, LVT_MASKED);
    lapic_write(LAPIC_LVT_LINT1, LVT_MASKED);
    lapic_write(LAPIC_LVT_THERMAL, LVT_MASKED);
    lapic_write(LAPIC_LVT_PERF, LVT_MASKED);
    lapic_write(LAPIC_LVT_ERROR, LVT_MASKED);

    /* 7. Clear Error Status Register */
    lapic_write(LAPIC_ESR, 0);
    lapic_write(LAPIC_ESR, 0);

    /* 8. Send EOI to clear any lingering in-service interrupts */
    lapic_eoi();
}

uint32_t lapic_get_id(void) {
    if (!g_lapic_regs) return 0;
    return (lapic_read(LAPIC_ID) >> 24) & 0xFF;
}

void lapic_eoi(void) {
    if (g_lapic_regs) {
        lapic_write(LAPIC_EOI, 0);
    }
}

void lapic_send_ipi(uint32_t dest_apic_id, uint32_t flags, uint8_t vector) {
    while (lapic_read(LAPIC_ICR_LOW) & ICR_DELIVS) {
        cpu_pause();
    }
    lapic_write(LAPIC_ICR_HIGH, dest_apic_id << 24);
    lapic_write(LAPIC_ICR_LOW, flags | vector);
}

void lapic_send_init(uint32_t dest_apic_id) {
    lapic_send_ipi(dest_apic_id, ICR_INIT | ICR_PHYSICAL | ICR_ASSERT | ICR_EDGE, 0);
}

void lapic_send_sipi(uint32_t dest_apic_id, uint8_t vector) {
    lapic_send_ipi(dest_apic_id, ICR_STARTUP | ICR_PHYSICAL | ICR_ASSERT | ICR_EDGE, vector);
}

static uint32_t g_lapic_timer_init_count = 0;

void lapic_timer_init(uint32_t frequency_hz) {
    if (!g_lapic_regs || frequency_hz == 0) return;

    /* Set divider to 16 */
    lapic_write(LAPIC_TIMER_DIV_CONFIG, 0x03);

    /* Measure ticks over 10 ms using PIT */
    lapic_write(LAPIC_TIMER_INIT_COUNT, 0xFFFFFFFF);
    pit_delay_ms(10);
    uint32_t ticks_10ms = 0xFFFFFFFF - lapic_read(LAPIC_TIMER_CURR_COUNT);

    /* Stop timer */
    lapic_write(LAPIC_TIMER_INIT_COUNT, 0);

    uint64_t ticks_per_sec = (uint64_t)ticks_10ms * 100ULL;
    uint32_t init_count = (uint32_t)(ticks_per_sec / frequency_hz);
    if (init_count == 0) init_count = 10000;
    g_lapic_timer_init_count = init_count;

    /* Configure periodic timer interrupt */
    lapic_write(LAPIC_LVT_TIMER, LVT_TIMER_PERIODIC | VEC_APIC_TIMER);
    lapic_write(LAPIC_TIMER_DIV_CONFIG, 0x03);
    lapic_write(LAPIC_TIMER_INIT_COUNT, init_count);

    kprintf("[APIC] LAPIC timer calibrated: %u Hz (init_count=%u, ticks/sec=%lu)\n",
            frequency_hz, init_count, ticks_per_sec);
}

void lapic_timer_init_ap(void) {
    if (!g_lapic_regs || g_lapic_timer_init_count == 0) return;
    lapic_write(LAPIC_TIMER_DIV_CONFIG, 0x03);
    lapic_write(LAPIC_LVT_TIMER, LVT_TIMER_PERIODIC | VEC_APIC_TIMER);
    lapic_write(LAPIC_TIMER_INIT_COUNT, g_lapic_timer_init_count);
}

void lapic_timer_stop(void) {
    if (g_lapic_regs) {
        lapic_write(LAPIC_TIMER_INIT_COUNT, 0);
        lapic_write(LAPIC_LVT_TIMER, LVT_MASKED);
    }
}

void ioapic_init(void) {
    /* 1. Disable legacy 8259 PIC by masking all lines */
    outb(0x21, 0xFF);
    outb(0xA1, 0xFF);

    if (g_acpi_info.ioapic_count == 0) {
        kprintf("[IOAPIC] WARNING: No I/O APIC discovered via MADT.\n");
        return;
    }

    uint64_t ioapic_phys = g_acpi_info.ioapic_phys[0];
    g_ioapic_regs = (volatile uint32_t *)phys_to_virt(ioapic_phys);

    uint32_t ver_reg = ioapic_read(0x01);
    uint32_t max_redirection_entries = ((ver_reg >> 16) & 0xFF) + 1;

    kprintf("[IOAPIC] I/O APIC #0 at 0x%lx initialized (%u redirection entries)\n",
            ioapic_phys, max_redirection_entries);

    /* Mask all redirection table entries initially */
    for (uint32_t i = 0; i < max_redirection_entries; i++) {
        ioapic_mask_irq(i);
    }
}

bool ioapic_route_irq(uint8_t irq, uint8_t vector, uint32_t dest_apic_id) {
    if (!g_ioapic_regs || dest_apic_id > 255) return false;

    /* Check for Interrupt Source Override (ISO) */
    uint32_t gsi = irq;
    uint32_t flags = 0;

    for (uint32_t i = 0; i < g_acpi_info.iso_count; i++) {
        if (g_acpi_info.isos[i].source == irq) {
            gsi = g_acpi_info.isos[i].gsi;
            /* Polarity and trigger flags from ACPI ISO */
            if ((g_acpi_info.isos[i].flags & 0x03) == 0x03) {
                flags |= (1 << 13); /* Active low */
            }
            if ((g_acpi_info.isos[i].flags & 0x0C) == 0x0C) {
                flags |= (1 << 15); /* Level triggered */
            }
            break;
        }
    }

    uint32_t first_gsi = g_acpi_info.ioapic_gsi_base[0];
    if (gsi < first_gsi) return false;
    uint32_t pin = gsi - first_gsi;
    if (pin > ((ioapic_read(0x01) >> 16) & 0xff)) return false;
    uint32_t low = vector | flags;
    uint32_t high = (dest_apic_id << 24);

    ioapic_write(0x11 + 2 * pin, high);
    ioapic_write(0x10 + 2 * pin, low);
    return true;
}

void ioapic_mask_irq(uint8_t irq) {
    if (!g_ioapic_regs) return;
    uint32_t low = ioapic_read(0x10 + 2 * irq);
    low |= (1 << 16); /* Mask bit */
    ioapic_write(0x10 + 2 * irq, low);
}

void ioapic_unmask_irq(uint8_t irq) {
    if (!g_ioapic_regs) return;
    uint32_t low = ioapic_read(0x10 + 2 * irq);
    low &= ~(1 << 16); /* Clear mask bit */
    ioapic_write(0x10 + 2 * irq, low);
}
