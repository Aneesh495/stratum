#ifndef STRATUM_KERNEL_APIC_H
#define STRATUM_KERNEL_APIC_H

#include <kernel/types.h>
#include <kernel/acpi.h>

/* Local APIC Register Offsets */
#define LAPIC_ID                0x020
#define LAPIC_VERSION           0x030
#define LAPIC_TPR               0x080
#define LAPIC_APR               0x090
#define LAPIC_PPR               0x0A0
#define LAPIC_EOI               0x0B0
#define LAPIC_RRD               0x0C0
#define LAPIC_LDR               0x0D0
#define LAPIC_DFR               0x0E0
#define LAPIC_SVR               0x0F0
#define LAPIC_ESR               0x280
#define LAPIC_ICR_LOW           0x300
#define LAPIC_ICR_HIGH          0x310
#define LAPIC_LVT_TIMER         0x320
#define LAPIC_LVT_THERMAL       0x330
#define LAPIC_LVT_PERF          0x340
#define LAPIC_LVT_LINT0         0x350
#define LAPIC_LVT_LINT1         0x360
#define LAPIC_LVT_ERROR         0x370
#define LAPIC_TIMER_INIT_COUNT  0x380
#define LAPIC_TIMER_CURR_COUNT  0x390
#define LAPIC_TIMER_DIV_CONFIG  0x3E0

/* ICR bits */
#define ICR_FIXED               (0x0 << 8)
#define ICR_LOWEST_PRIO         (0x1 << 8)
#define ICR_SMI                 (0x2 << 8)
#define ICR_NMI                 (0x4 << 8)
#define ICR_INIT                (0x5 << 8)
#define ICR_STARTUP             (0x6 << 8)
#define ICR_PHYSICAL            (0x0 << 11)
#define ICR_LOGICAL             (0x1 << 11)
#define ICR_DELIVS             (0x1 << 12)
#define ICR_DEASSERT            (0x0 << 14)
#define ICR_ASSERT              (0x1 << 14)
#define ICR_EDGE                (0x0 << 15)
#define ICR_LEVEL               (0x1 << 15)
#define ICR_NO_SHORTHAND        (0x0 << 18)
#define ICR_SELF                (0x1 << 18)
#define ICR_ALL_INCL_SELF       (0x2 << 18)
#define ICR_ALL_EXCL_SELF       (0x3 << 18)

/* LVT Timer Modes */
#define LVT_TIMER_ONESHOT       (0x0 << 17)
#define LVT_TIMER_PERIODIC      (0x1 << 17)
#define LVT_TIMER_TSC_DEADLINE  (0x2 << 17)
#define LVT_MASKED              (0x1 << 16)

/* Standard Vectors */
#define VEC_APIC_TIMER          0x20
#define VEC_UART_RX             0x24
#define VEC_IPI_RESCHED         0x30
#define VEC_IPI_TLB             0xFD
#define VEC_IPI_HALT            0xFE
#define VEC_SPURIOUS            0xFF

void     lapic_init(void);
uint32_t lapic_get_id(void);
void     lapic_eoi(void);
void     lapic_send_ipi(uint32_t dest_apic_id, uint32_t flags, uint8_t vector);
void     lapic_send_init(uint32_t dest_apic_id);
void     lapic_send_sipi(uint32_t dest_apic_id, uint8_t vector);
void     lapic_timer_init(uint32_t frequency_hz);
void     lapic_timer_init_ap(void);
void     lapic_timer_stop(void);
void     pit_delay_ms(uint32_t ms);

/* I/O APIC */
void     ioapic_init(void);
bool     ioapic_route_irq(uint8_t irq, uint8_t vector, uint32_t dest_apic_id);
void     ioapic_mask_irq(uint8_t irq);
void     ioapic_unmask_irq(uint8_t irq);

#endif /* STRATUM_KERNEL_APIC_H */
