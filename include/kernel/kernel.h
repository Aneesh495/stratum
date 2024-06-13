#ifndef STRATUM_KERNEL_KERNEL_H
#define STRATUM_KERNEL_KERNEL_H

#include <kernel/types.h>
#include <kernel/string.h>
#include <shared/errno.h>

#define KERNEL_NAME    "Stratum"
#define KERNEL_VERSION "0.1.0"

/* Kernel console and printing */
int kprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void kputs(const char *s);
void kconsole_write(const char *bytes, size_t size, bool mirror);
void kputchar(char c);

/* Panic and assertion */
__attribute__((noreturn)) void panic(const char *fmt, ...);

#define kassert(cond) do { \
    if (!(cond)) { \
        panic("Assertion failed: %s at %s:%d in %s\n", #cond, __FILE__, __LINE__, __func__); \
    } \
} while (0)

#endif /* STRATUM_KERNEL_KERNEL_H */
