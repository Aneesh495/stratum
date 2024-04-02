#include <kernel/idt.h>
#include <kernel/kernel.h>
#include <kernel/string.h>
#include <kernel/x86_64.h>
#include <kernel/apic.h>
#include <kernel/process.h>

static idt_entry_t g_idt[IDT_ENTRIES];
static idt_ptr_t g_idt_ptr;
static irq_handler_t g_irq_handlers[IDT_ENTRIES];

extern uint64_t isr_stub_table[256];

static const char *g_exception_names[32] = {
    "Divide Error (#DE)",
    "Debug Exception (#DB)",
    "NMI Interrupt",
    "Breakpoint (#BP)",
    "Overflow (#OF)",
    "BOUND Range Exceeded (#BR)",
    "Invalid Opcode (#UD)",
    "Device Not Available (#NM)",
    "Double Fault (#DF)",
    "Coprocessor Segment Overrun",
    "Invalid TSS (#TS)",
    "Segment Not Present (#NP)",
    "Stack-Segment Fault (#SS)",
    "General Protection Fault (#GP)",
    "Page Fault (#PF)",
    "Reserved",
    "x87 FPU Floating-Point Error (#MF)",
    "Alignment Check (#AC)",
    "Machine Check (#MC)",
    "SIMD Floating-Point Exception (#XM)",
    "Virtualization Exception (#VE)",
    "Control Protection Exception (#CP)",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Hypervisor Injection Exception (#HV)",
    "VMM Communication Exception (#VC)",
    "Security Exception (#SX)",
    "Reserved"
};

void idt_set_gate(uint8_t vector, void *handler, uint16_t selector, uint8_t ist, uint8_t flags) {
    uint64_t addr = (uint64_t)handler;
    g_idt[vector].offset_low = (uint16_t)(addr & 0xFFFF);
    g_idt[vector].selector = selector;
    g_idt[vector].ist = ist & 0x07;
    g_idt[vector].type_attr = flags;
    g_idt[vector].offset_mid = (uint16_t)((addr >> 16) & 0xFFFF);
    g_idt[vector].offset_high = (uint32_t)((addr >> 32) & 0xFFFFFFFF);
    g_idt[vector].zero = 0;
}

int register_interrupt_handler(uint8_t vector, irq_handler_t handler) {
    g_irq_handlers[vector] = handler;
    return 0;
}

void dump_interrupt_frame(const interrupt_frame_t *frame) {
    uint64_t cr2 = read_cr2();
    uint64_t cr3 = read_cr3();

    kprintf("\n--- INTERRUPT FRAME DUMP ---\n");
    kprintf("Vector: %lu (%s), Error Code: 0x%lx\n",
            frame->vector,
            (frame->vector < 32) ? g_exception_names[frame->vector] : "User/Hardware IRQ",
            frame->error_code);
    kprintf("RIP: 0x%016lx  CS:  0x%04lx  RFLAGS: 0x%016lx\n", frame->rip, frame->cs, frame->rflags);
    kprintf("RSP: 0x%016lx  SS:  0x%04lx  CR2:    0x%016lx  CR3: 0x%016lx\n",
            frame->rsp, frame->ss, cr2, cr3);
    kprintf("RAX: 0x%016lx  RBX: 0x%016lx  RCX:    0x%016lx  RDX: 0x%016lx\n",
            frame->rax, frame->rbx, frame->rcx, frame->rdx);
    kprintf("RSI: 0x%016lx  RDI: 0x%016lx  RBP:    0x%016lx\n",
            frame->rsi, frame->rdi, frame->rbp);
    kprintf("R8:  0x%016lx  R9:  0x%016lx  R10:    0x%016lx  R11: 0x%016lx\n",
            frame->r8, frame->r9, frame->r10, frame->r11);
    kprintf("R12: 0x%016lx  R13: 0x%016lx  R14:    0x%016lx  R15: 0x%016lx\n",
            frame->r12, frame->r13, frame->r14, frame->r15);
    kprintf("----------------------------\n");
}

void exception_dispatch(interrupt_frame_t *frame) {
    if (frame->vector < 32) {
        /* Check if a registered exception handler exists (e.g. vector 14 Page Fault) */
        if (g_irq_handlers[frame->vector]) {
            g_irq_handlers[frame->vector](frame);
            return;
        }

        /* CPU Exception */
        dump_interrupt_frame(frame);

        /* Check if exception occurred in user mode (Ring 3) */
        if ((frame->cs & 3) == 3) {
            kprintf("[FAULT] User mode exception in ring 3. Terminating faulting task.\n");
            process_exit(128 + (int)frame->vector);
        }

        panic("CPU Exception %lu: %s (err=0x%lx, rip=0x%lx)",
              frame->vector, g_exception_names[frame->vector],
              frame->error_code, frame->rip);
    } else {
        /* Send Local APIC EOI before dispatching handler to permit preemptive context switches */
        if (frame->vector != 0xFF) {
            lapic_eoi();
        }
        /* Hardware IRQ or IPI */
        if (g_irq_handlers[frame->vector]) {
            g_irq_handlers[frame->vector](frame);
        }
    }
}

void idt_load(void) {
    __asm__ volatile("lidt %0" : : "m"(g_idt_ptr));
}

void idt_init(void) {
    memset(g_idt, 0, sizeof(g_idt));
    memset(g_irq_handlers, 0, sizeof(g_irq_handlers));

    /* Install all 256 gates using stubs from interrupts.S */
    for (int i = 0; i < IDT_ENTRIES; i++) {
        uint8_t ist = 0;
        if (i == 8) {
            ist = 1; /* Double fault uses IST1 */
        } else if (i == 2) {
            ist = 2; /* NMI uses IST2 */
        }
        /* 0x8E = Present | DPL 0 | 64-bit Interrupt Gate */
        idt_set_gate(i, (void *)isr_stub_table[i], 0x08, ist, 0x8E);
    }

    g_idt_ptr.limit = sizeof(g_idt) - 1;
    g_idt_ptr.base = (uint64_t)&g_idt[0];

    idt_load();

    kprintf("[CPU] IDT initialized with 256 vector gates (IST1 Double Fault, IST2 NMI)\n");
}
