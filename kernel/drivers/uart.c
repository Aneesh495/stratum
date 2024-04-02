#include <kernel/uart.h>
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

static bool uart_initialized = false;

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
    return (inb(COM1_PORT + UART_LSR) & UART_LSR_DR) != 0;
}

int uart_getc_nonblocking(void) {
    if (!uart_initialized) {
        uart_init();
    }
    if (uart_input_ready()) {
        return (int)inb(COM1_PORT + UART_RBR);
    }
    return -1;
}

char uart_getc_blocking(void) {
    int c;
    while ((c = uart_getc_nonblocking()) < 0) {
        cpu_pause();
    }
    return (char)c;
}
