#ifndef STRATUM_KERNEL_X86_64_H
#define STRATUM_KERNEL_X86_64_H

#include <kernel/types.h>

/* Standard MSRs */
#define MSR_APIC_BASE       0x0000001B
#define MSR_EFER            0xC0000080
#define MSR_STAR            0xC0000081
#define MSR_LSTAR           0xC0000082
#define MSR_CSTAR           0xC0000083
#define MSR_SFMASK          0xC0000084
#define MSR_FS_BASE         0xC0000100
#define MSR_GS_BASE         0xC0000101
#define MSR_KERNEL_GS_BASE  0xC0000102

/* EFER bit flags */
#define EFER_SCE            (1ULL << 0)   /* Syscall Extensions */
#define EFER_LME            (1ULL << 8)   /* Long Mode Enable */
#define EFER_LMA            (1ULL << 10)  /* Long Mode Active */
#define EFER_NXE            (1ULL << 11)  /* No-Execute Enable */

/* CR0 bit flags */
#define CR0_PE              (1ULL << 0)   /* Protected Mode Enable */
#define CR0_MP              (1ULL << 1)   /* Monitor Coprocessor */
#define CR0_EM              (1ULL << 2)   /* Emulation */
#define CR0_TS              (1ULL << 3)   /* Task Switched */
#define CR0_ET              (1ULL << 4)   /* Extension Type */
#define CR0_NE              (1ULL << 5)   /* Numeric Error */
#define CR0_WP              (1ULL << 16)  /* Write Protect */
#define CR0_PG              (1ULL << 31)  /* Paging */

/* CR4 bit flags */
#define CR4_PAE             (1ULL << 5)   /* Physical Address Extension */
#define CR4_PGE             (1ULL << 7)   /* Page Global Enable */
#define CR4_PCE             (1ULL << 8)   /* Performance Counter Enable */
#define CR4_OSFXSR          (1ULL << 9)   /* OS Support for FXSAVE/FXRSTOR */
#define CR4_OSXMMEXCPT      (1ULL << 10)  /* OS Support for Unmasked SIMD Exceptions */
#define CR4_OSXSAVE         (1ULL << 18)  /* OS Support for XSAVE/XRSTOR */
#define CR4_FSGSBASE        (1ULL << 16)  /* Enable RDFSBASE/RDGSBASE/etc. */

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outw(uint16_t port, uint16_t val) {
    __asm__ volatile("outw %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint16_t inw(uint16_t port) {
    uint16_t ret;
    __asm__ volatile("inw %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outl(uint16_t port, uint32_t val) {
    __asm__ volatile("outl %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint32_t inl(uint16_t port) {
    uint32_t ret;
    __asm__ volatile("inl %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void io_wait(void) {
    outb(0x80, 0);
}

static inline void cli(void) {
    __asm__ volatile("cli" ::: "memory");
}

static inline void sti(void) {
    __asm__ volatile("sti" ::: "memory");
}

static inline void hlt(void) {
    __asm__ volatile("hlt");
}

static inline void cpu_pause(void) {
    __asm__ volatile("pause" ::: "memory");
}

static inline uint64_t read_rflags(void) {
    uint64_t rflags;
    __asm__ volatile("pushfq; popq %0" : "=r"(rflags) :: "memory");
    return rflags;
}

static inline uint64_t local_irq_save(void) {
    uint64_t flags = read_rflags();
    cli();
    return flags;
}

static inline void local_irq_restore(uint64_t flags) {
    if (flags & (1ULL << 9)) {
        sti();
    } else {
        cli();
    }
}

static inline uint64_t read_cr0(void) {
    uint64_t val;
    __asm__ volatile("mov %%cr0, %0" : "=r"(val));
    return val;
}

static inline void write_cr0(uint64_t val) {
    __asm__ volatile("mov %0, %%cr0" : : "r"(val) : "memory");
}

static inline uint64_t read_cr2(void) {
    uint64_t val;
    __asm__ volatile("mov %%cr2, %0" : "=r"(val));
    return val;
}

static inline uint64_t read_cr3(void) {
    uint64_t val;
    __asm__ volatile("mov %%cr3, %0" : "=r"(val));
    return val;
}

static inline void write_cr3(uint64_t val) {
    __asm__ volatile("mov %0, %%cr3" : : "r"(val) : "memory");
}

static inline uint64_t read_cr4(void) {
    uint64_t val;
    __asm__ volatile("mov %%cr4, %0" : "=r"(val));
    return val;
}

static inline void write_cr4(uint64_t val) {
    __asm__ volatile("mov %0, %%cr4" : : "r"(val) : "memory");
}

static inline void invlpg(uint64_t vaddr) {
    __asm__ volatile("invlpg (%0)" : : "r"(vaddr) : "memory");
}

static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t low, high;
    __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return ((uint64_t)high << 32) | low;
}

static inline void wrmsr(uint32_t msr, uint64_t val) {
    uint32_t low = (uint32_t)val;
    uint32_t high = (uint32_t)(val >> 32);
    __asm__ volatile("wrmsr" : : "a"(low), "d"(high), "c"(msr));
}

static inline uint64_t rdtsc(void) {
    uint32_t low, high;
    __asm__ volatile("rdtsc" : "=a"(low), "=d"(high));
    return ((uint64_t)high << 32) | low;
}

#endif /* STRATUM_KERNEL_X86_64_H */
