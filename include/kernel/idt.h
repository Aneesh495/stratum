#ifndef STRATUM_KERNEL_IDT_H
#define STRATUM_KERNEL_IDT_H

#include <kernel/types.h>

#define IDT_ENTRIES 256

/* Interrupt Frame pushed by interrupts.S */
typedef struct {
    /* Pushed by common assembly stub */
    uint64_t r15;
    uint64_t r14;
    uint64_t r13;
    uint64_t r12;
    uint64_t r11;
    uint64_t r10;
    uint64_t r9;
    uint64_t r8;
    uint64_t rbp;
    uint64_t rdi;
    uint64_t rsi;
    uint64_t rdx;
    uint64_t rcx;
    uint64_t rbx;
    uint64_t rax;

    /* Pushed by specific isr stub */
    uint64_t vector;
    uint64_t error_code;

    /* Pushed by CPU hardware */
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
} __attribute__((packed)) interrupt_frame_t;

typedef struct {
    uint16_t offset_low;   /* Offset bits 0..15 */
    uint16_t selector;     /* Code segment selector */
    uint8_t  ist;          /* Bits 0..2: IST index (1..7), rest 0 */
    uint8_t  type_attr;    /* Type and attributes (P, DPL, Gate type) */
    uint16_t offset_mid;   /* Offset bits 16..31 */
    uint32_t offset_high;  /* Offset bits 32..63 */
    uint32_t zero;         /* Reserved */
} __attribute__((packed)) idt_entry_t;

typedef struct {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed)) idt_ptr_t;

typedef void (*irq_handler_t)(interrupt_frame_t *frame);

void idt_init(void);
void idt_load(void);
void idt_enable_stack_probe(void);
void idt_set_gate(uint8_t vector, void *handler, uint16_t selector, uint8_t ist, uint8_t flags);
int  register_interrupt_handler(uint8_t vector, irq_handler_t handler);
void exception_dispatch(interrupt_frame_t *frame);
void dump_interrupt_frame(const interrupt_frame_t *frame);

#endif /* STRATUM_KERNEL_IDT_H */
