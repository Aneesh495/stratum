#ifndef STRATUM_KERNEL_UART_H
#define STRATUM_KERNEL_UART_H

#include <kernel/types.h>

#define COM1_PORT 0x3F8

void uart_init(void);
void uart_enable_rx_irq(void);
bool uart_rx_uses_irq(void);
void uart_putc(char c);
void uart_puts(const char *s);
void uart_write(const char *buf, size_t count);
bool uart_input_ready(void);
int  uart_getc_nonblocking(void);
char uart_getc_blocking(void);

#endif /* STRATUM_KERNEL_UART_H */
