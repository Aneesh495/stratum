#include <kernel/kernel.h>
#include <kernel/uart.h>
#include <kernel/fb.h>
#include <kernel/x86_64.h>
#include <stdarg.h>

void kputchar(char c) {
    uart_putc(c);
    fb_putchar(c);
}

void kputs(const char *s) {
    if (!s) return;
    while (*s) {
        kputchar(*s++);
    }
}

#include <kernel/spinlock.h>

static spinlock_t g_kprintf_lock = SPINLOCK_INIT;

void kconsole_write(const char *bytes, size_t size, bool mirror) {
    uint64_t flags;
    spin_lock_irqsave(&g_kprintf_lock, &flags);
    if (mirror) {
        for (size_t i = 0; i < size; i++) kputchar(bytes[i]);
    } else {
        uart_write(bytes, size);
    }
    spin_unlock_irqrestore(&g_kprintf_lock, flags);
}

int kprintf(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int len = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    uint64_t flags;
    spin_lock_irqsave(&g_kprintf_lock, &flags);
    kputs(buf);
    spin_unlock_irqrestore(&g_kprintf_lock, flags);

    return len;
}

void panic(const char *fmt, ...) {
    cli();

    kputs("\n=======================================================\n");
    kputs("KERNEL PANIC: ");

    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    kputs(buf);
    kputs("\nSystem halted.\n");
    kputs("=======================================================\n");

    while (1) {
        hlt();
    }
}
