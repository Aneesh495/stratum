#ifndef STRATUM_KERNEL_VMM_H
#define STRATUM_KERNEL_VMM_H

#include <kernel/types.h>
#include <kernel/idt.h>
#include <shared/boot_info.h>

#define PTE_PRESENT   (1ULL << 0)
#define PTE_WRITABLE  (1ULL << 1)
#define PTE_USER      (1ULL << 2)
#define PTE_PWT       (1ULL << 3)
#define PTE_PCD       (1ULL << 4)
#define PTE_ACCESSED  (1ULL << 5)
#define PTE_DIRTY     (1ULL << 6)
#define PTE_HUGE      (1ULL << 7)
#define PTE_GLOBAL    (1ULL << 8)
#define PTE_COW       (1ULL << 9)   /* Software Copy-On-Write bit */
#define PTE_SHARED    (1ULL << 10)  /* Shared user page, exempt from COW */
#define PTE_NX        (1ULL << 63)
#define PTE_ADDR_MASK 0x000FFFFFFFFFF000ULL
#define PTE_FLAGS_MASK (0x0000000000000FFFULL | PTE_NX)

#define PML4_INDEX(va) (((va) >> 39) & 0x1FF)
#define PDPT_INDEX(va) (((va) >> 30) & 0x1FF)
#define PD_INDEX(va)   (((va) >> 21) & 0x1FF)
#define PT_INDEX(va)   (((va) >> 12) & 0x1FF)

typedef struct {
    uint64_t entries[512];
} __attribute__((aligned(4096))) page_table_t;

typedef page_table_t pml4_t;

extern pml4_t *g_kernel_pml4;

void    vmm_init(const boot_handoff_t *handoff);
pml4_t *vmm_create_address_space(void);
pml4_t *vmm_clone_address_space(pml4_t *src);
void    vmm_destroy_address_space(pml4_t *pml4);
void    vmm_switch_address_space(pml4_t *pml4);

int     vmm_map_page(pml4_t *pml4, uint64_t vaddr, uint64_t paddr, uint64_t flags);
int     vmm_unmap_page(pml4_t *pml4, uint64_t vaddr);
int     vmm_get_mapping(pml4_t *pml4, uint64_t vaddr, uint64_t *out_paddr, uint64_t *out_flags);
int     vmm_protect(pml4_t *pml4, uint64_t vaddr, uint64_t flags);

void    vmm_page_fault_handler(interrupt_frame_t *frame);

#endif /* STRATUM_KERNEL_VMM_H */
