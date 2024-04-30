#include <kernel/uart.h>
#include <kernel/apic.h>
#include <kernel/event.h>
#include <kernel/idt.h>
#include <kernel/spinlock.h>
#include <kernel/x86_64.h>

#define UART_THR 0 /* Transmitter Holding Buffer */
#define UART_RBR 0 /* Receiver Buffer */
#define UART_DLL 0 /* Divisor Latch Low Byte */
#define UART_IER 1 /* Interrupt Enable Register */
#define UART_DLH 1 /* Divisor Latch High Byte */
#define UART_IIR 2 /* Interrupt Identification Register */
#define UART_FCR 2 /* FIFO Control Register */
#define UART_LCR 3 /* Line Control Register */
#define UART_MCR 4 /* Modem Control Register */
#define UART_LSR 5 /* Line Status Register */
#define UART_MSR 6 /* Modem Status Register */
#define UART_SCR 7 /* Scratch Register */

#define UART_LSR_DR   (1 << 0) /* Data Ready */
#define UART_LSR_THRE (1 << 5) /* Transmitter Holding Register Empty */

#define UART_RX_CAPACITY 512

static bool uart_initialized = false;
static bool rx_irq;
static uint8_t rx_buffer[UART_RX_CAPACITY];
static unsigned rx_read, rx_count;
static spinlock_t rx_lock = SPINLOCK_INIT;

static void uart_receive_interrupt(interrupt_frame_t *frame) {
    (void)frame;
    uint64_t flags;
    spin_lock_irqsave(&rx_lock, &flags);
    bool empty = rx_count == 0;
    for (unsigned budget = 0; budget < 64 && (inb(COM1_PORT + UART_LSR) & UART_LSR_DR); budget++) {
        uint8_t value = inb(COM1_PORT + UART_RBR);
        if (rx_count == UART_RX_CAPACITY) continue;
        rx_buffer[(rx_read + rx_count++) % UART_RX_CAPACITY] = value;
    }
    bool readable = empty && rx_count;
    spin_unlock_irqrestore(&rx_lock, flags);
    if (readable) readiness_notify();
}

void uart_enable_rx_irq(void) {
    register_interrupt_handler(VEC_UART_RX, uart_receive_interrupt);
    if (!ioapic_route_irq(4, VEC_UART_RX, lapic_get_id())) return;
    rx_irq = true;
    outb(COM1_PORT + UART_FCR, 0x01); /* One-byte receive trigger. */
    outb(COM1_PORT + UART_IER, 0x01);
}

bool uart_rx_uses_irq(void) {
    return rx_irq;
}

void uart_init(void) {
    /* Disable all interrupts during setup */
    outb(COM1_PORT + UART_IER, 0x00);

    /* Enable DLAB (set baud rate divisor) */
    outb(COM1_PORT + UART_LCR, 0x80);

    /* Set divisor to 1 (115200 baud): DLL = 1, DLH = 0 */
    outb(COM1_PORT + UART_DLL, 0x01);
    outb(COM1_PORT + UART_DLH, 0x00);

    /* 8 bits, no parity, one stop bit (8N1) */
    outb(COM1_PORT + UART_LCR, 0x03);

    /* Enable FIFO, clear RX/TX FIFOs, 14-byte threshold */
    outb(COM1_PORT + UART_FCR, 0xC7);

    /* Auxiliary Output 2 set (enables IRQ line on hardware), RTS/DTR set */
    outb(COM1_PORT + UART_MCR, 0x0B);

    uart_initialized = true;
}

static inline bool uart_can_transmit(void) {
    return (inb(COM1_PORT + UART_LSR) & UART_LSR_THRE) != 0;
}

void uart_putc(char c) {
    if (!uart_initialized) {
        uart_init();
    }
    if (c == '\n') {
        while (!uart_can_transmit()) {
            cpu_pause();
        }
        outb(COM1_PORT + UART_THR, '\r');
    }
    while (!uart_can_transmit()) {
        cpu_pause();
    }
    outb(COM1_PORT + UART_THR, (uint8_t)c);
}

void uart_write(const char *buf, size_t count) {
    if (!buf || count == 0) return;
    for (size_t i = 0; i < count; i++) {
        uart_putc(buf[i]);
    }
}

void uart_puts(const char *s) {
    if (!s) return;
    while (*s) {
        uart_putc(*s++);
    }
}

bool uart_input_ready(void) {
    if (!rx_irq) return (inb(COM1_PORT + UART_LSR) & UART_LSR_DR) != 0;
    uint64_t flags;
    spin_lock_irqsave(&rx_lock, &flags);
    bool ready = rx_count != 0;
    spin_unlock_irqrestore(&rx_lock, flags);
    return ready;
}

int uart_getc_nonblocking(void) {
    if (!uart_initialized) {
        uart_init();
    }
    if (!rx_irq) return uart_input_ready() ? (int)inb(COM1_PORT + UART_RBR) : -1;
    uint64_t flags;
    spin_lock_irqsave(&rx_lock, &flags);
    int value = -1;
    if (rx_count) {
        value = rx_buffer[rx_read];
        rx_read = (rx_read + 1) % UART_RX_CAPACITY;
        rx_count--;
    }
    spin_unlock_irqrestore(&rx_lock, flags);
    return value;
}

char uart_getc_blocking(void) {
    int c;
    while ((c = uart_getc_nonblocking()) < 0) {
        cpu_pause();
    }
    return (char)c;
}
