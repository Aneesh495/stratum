#ifndef STRATUM_KERNEL_GDT_H
#define STRATUM_KERNEL_GDT_H

#include <kernel/types.h>

#define GDT_KERNEL_CODE  0x08
#define GDT_KERNEL_DATA  0x10
#define GDT_USER_DATA    0x18
#define GDT_USER_CODE    0x20
#define GDT_TSS          0x28

/* Selectors with RPL */
#define KERNEL_CS        GDT_KERNEL_CODE
#define KERNEL_DS        GDT_KERNEL_DATA
#define USER_DS          (GDT_USER_DATA | 3)
#define USER_CS          (GDT_USER_CODE | 3)
#define TSS_SELECTOR     GDT_TSS

typedef struct {
    uint32_t reserved0;
    uint64_t rsp0;       /* Ring 0 stack pointer */
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist1;       /* IST 1: Double Fault Stack */
    uint64_t ist2;       /* IST 2: NMI Stack */
    uint64_t ist3;       /* IST 3: Machine Check / Debug Stack */
    uint64_t ist4;
    uint64_t ist5;
    uint64_t ist6;
    uint64_t ist7;
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iopb_offset;
} __attribute__((packed)) tss_t;

typedef struct {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed)) gdt_ptr_t;

typedef gdt_ptr_t gdt_desc_t;

struct cpu;

void gdt_init(void);
void gdt_init_ap(struct cpu *cpu);
void gdt_set_kernel_stack(uint64_t rsp0);
const tss_t *gdt_current_tss(void);

#endif /* STRATUM_KERNEL_GDT_H */
