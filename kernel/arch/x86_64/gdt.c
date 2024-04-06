#include <kernel/gdt.h>
#include <kernel/string.h>
#include <kernel/kernel.h>

#define GDT_ENTRIES 7

static uint64_t g_gdt[GDT_ENTRIES];
static gdt_ptr_t g_gdt_ptr;
static tss_t g_tss;

/* Dedicated interrupt stacks */
static uint8_t g_double_fault_stack[16384] __attribute__((aligned(16)));
static uint8_t g_nmi_stack[16384] __attribute__((aligned(16)));
static uint8_t g_priv_stack[16384] __attribute__((aligned(16)));

static void gdt_set_gate(int index, uint64_t base, uint64_t limit, uint8_t access, uint8_t flags) {
    uint64_t desc = 0;
    desc |= (limit & 0xFFFF);
    desc |= (base & 0xFFFFFF) << 16;
    desc |= ((uint64_t)access) << 40;
    desc |= ((limit >> 16) & 0x0F) << 48;
    desc |= (((uint64_t)flags) & 0x0F) << 52;
    desc |= ((base >> 24) & 0xFF) << 56;
    g_gdt[index] = desc;
}

static void gdt_set_tss_gate(int index, uint64_t base, uint32_t limit) {
    /* Low 8 bytes */
    gdt_set_gate(index, base & 0xFFFFFFFF, limit, 0x89, 0x00);
    /* High 8 bytes: bits 32..63 of base address */
    g_gdt[index + 1] = (base >> 32);
}

extern void gdt_reload_segments(void);

void gdt_init(void) {
    memset(g_gdt, 0, sizeof(g_gdt));
    memset(&g_tss, 0, sizeof(g_tss));

    /* Entry 0: Null Descriptor */
    g_gdt[0] = 0;

    /* Entry 1 (0x08): Kernel Code 64-bit: Base=0, Limit=0xFFFFF, Access=0x9A, Flags=0x2 (Long mode) */
    gdt_set_gate(1, 0, 0xFFFFF, 0x9A, 0x02);

    /* Entry 2 (0x10): Kernel Data 64-bit: Base=0, Limit=0xFFFFF, Access=0x92, Flags=0x0 */
    gdt_set_gate(2, 0, 0xFFFFF, 0x92, 0x00);

    /* Entry 3 (0x18): User Data 64-bit: Base=0, Limit=0xFFFFF, Access=0xF2, Flags=0x0 */
    gdt_set_gate(3, 0, 0xFFFFF, 0xF2, 0x00);

    /* Entry 4 (0x20): User Code 64-bit: Base=0, Limit=0xFFFFF, Access=0xFA, Flags=0x2 (Long mode) */
    gdt_set_gate(4, 0, 0xFFFFF, 0xFA, 0x02);

    /* Setup TSS and dedicated IST stacks */
    g_tss.rsp0 = (uint64_t)&g_priv_stack[sizeof(g_priv_stack)];
    g_tss.ist1 = (uint64_t)&g_double_fault_stack[sizeof(g_double_fault_stack)];
    g_tss.ist2 = (uint64_t)&g_nmi_stack[sizeof(g_nmi_stack)];
    g_tss.iopb_offset = sizeof(tss_t);

    /* Entry 5 & 6 (0x28): TSS descriptor (16 bytes in x86-64 long mode) */
    gdt_set_tss_gate(5, (uint64_t)&g_tss, sizeof(tss_t) - 1);

    g_gdt_ptr.limit = sizeof(g_gdt) - 1;
    g_gdt_ptr.base = (uint64_t)&g_gdt[0];

    /* Load GDT */
    __asm__ volatile("lgdt %0" : : "m"(g_gdt_ptr));

    /* Reload segment registers and CS via far return */
    __asm__ volatile(
        "pushq $0x08\n\t"
        "leaq .Lflush_cs(%%rip), %%rax\n\t"
        "pushq %%rax\n\t"
        "lretq\n\t"
        ".Lflush_cs:\n\t"
        "movw $0x10, %%ax\n\t"
        "movw %%ax, %%ds\n\t"
        "movw %%ax, %%es\n\t"
        "movw %%ax, %%ss\n\t"
        "movw $0x00, %%ax\n\t"
        "movw %%ax, %%fs\n\t"
        "movw %%ax, %%gs\n\t"
        :
        :
        : "rax", "memory"
    );

    /* Load Task Register (TSS) */
    __asm__ volatile("ltr %0" : : "r"((uint16_t)GDT_TSS));

    kprintf("[CPU] GDT and 64-bit TSS loaded (IST1 Double Fault, IST2 NMI active)\n");
}

#include <kernel/smp.h>

void gdt_set_kernel_stack(uint64_t rsp0) {
    cpu_t *cpu = smp_get_current_cpu();
    if (cpu) {
        cpu->kernel_stack = rsp0;
        if (!cpu->is_bsp) {
            cpu->tss.rsp0 = rsp0;
        } else {
            g_tss.rsp0 = rsp0;
        }
    } else {
        g_tss.rsp0 = rsp0;
    }
}

void gdt_init_ap(struct cpu *cpu) {
    if (!cpu) return;

    memset(cpu->gdt_entries, 0, sizeof(cpu->gdt_entries));
    memset(&cpu->tss, 0, sizeof(cpu->tss));

    /* Entry 0: Null Descriptor */
    cpu->gdt_entries[0] = 0;

    /* Entry 1 (0x08): Kernel Code 64-bit */
    uint64_t desc1 = 0;
    desc1 |= 0xFFFF;
    desc1 |= ((uint64_t)0x9A) << 40;
    desc1 |= (((uint64_t)0x02) & 0x0F) << 52;
    cpu->gdt_entries[1] = desc1;

    /* Entry 2 (0x10): Kernel Data 64-bit */
    uint64_t desc2 = 0;
    desc2 |= 0xFFFF;
    desc2 |= ((uint64_t)0x92) << 40;
    cpu->gdt_entries[2] = desc2;

    /* Entry 3 (0x18): User Data 64-bit */
    uint64_t desc3 = 0;
    desc3 |= 0xFFFF;
    desc3 |= ((uint64_t)0xF2) << 40;
    cpu->gdt_entries[3] = desc3;

    /* Entry 4 (0x20): User Code 64-bit */
    uint64_t desc4 = 0;
    desc4 |= 0xFFFF;
    desc4 |= ((uint64_t)0xFA) << 40;
    desc4 |= (((uint64_t)0x02) & 0x0F) << 52;
    cpu->gdt_entries[4] = desc4;

    /* Setup AP TSS */
    cpu->tss.rsp0 = cpu->kernel_stack_top;
    cpu->tss.ist1 = cpu->kernel_stack_top + 8192;
    cpu->tss.ist2 = cpu->kernel_stack_top + 16384;
    cpu->tss.iopb_offset = sizeof(tss_t);

    /* Entry 5 & 6 (0x28): TSS descriptor (16 bytes) */
    uint64_t base = (uint64_t)&cpu->tss;
    uint32_t limit = sizeof(tss_t) - 1;

    uint64_t tss_low = 0;
    tss_low |= (limit & 0xFFFF);
    tss_low |= (base & 0xFFFFFF) << 16;
    tss_low |= ((uint64_t)0x89) << 40;
    tss_low |= ((uint64_t)((limit >> 16) & 0x0F)) << 48;
    tss_low |= ((base >> 24) & 0xFF) << 56;

    cpu->gdt_entries[5] = tss_low;
    cpu->gdt_entries[6] = (base >> 32);

    cpu->gdt_desc.limit = sizeof(cpu->gdt_entries) - 1;
    cpu->gdt_desc.base = (uint64_t)&cpu->gdt_entries[0];

    /* Load AP GDT */
    __asm__ volatile("lgdt %0" : : "m"(cpu->gdt_desc));

    /* Reload segment registers */
    __asm__ volatile(
        "pushq $0x08\n\t"
        "leaq .Lflush_ap_cs(%%rip), %%rax\n\t"
        "pushq %%rax\n\t"
        "lretq\n\t"
        ".Lflush_ap_cs:\n\t"
        "movw $0x10, %%ax\n\t"
        "movw %%ax, %%ds\n\t"
        "movw %%ax, %%es\n\t"
        "movw %%ax, %%ss\n\t"
        "movw $0x00, %%ax\n\t"
        "movw %%ax, %%fs\n\t"
        "movw %%ax, %%gs\n\t"
        :
        :
        : "rax", "memory"
    );

    /* Load Task Register */
    __asm__ volatile("ltr %0" : : "r"((uint16_t)GDT_TSS));
}
