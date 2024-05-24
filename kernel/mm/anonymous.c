#include <kernel/process.h>
#include <kernel/futex.h>
#include <kernel/pmm.h>
#include <kernel/vmm.h>
#include <shared/errno.h>
#include <shared/syscall_nums.h>

#define ANON_BASE  0x0000400000000000ULL
#define ANON_LIMIT 0x0000400100000000ULL
#define ANON_MAX_LENGTH (16ULL * 1024 * 1024)

static bool valid_protection(unsigned protection) {
    return protection == PROT_READ || protection == (PROT_READ | PROT_WRITE);
}

static uint64_t page_length(uint64_t length) {
    return length && length <= ANON_MAX_LENGTH ? round_up(length, PAGE_SIZE) : 0;
}

static user_region_t *find_region(process_t *process, uint64_t address) {
    for (unsigned i = 0; i < PROCESS_ANON_REGIONS; i++) {
        user_region_t *region = &process->regions[i];
        if (address >= region->start && address - region->start < region->length) return region;
    }
    return NULL;
}

static int populate_shared(process_t *process, uint64_t start, uint64_t length, unsigned protection) {
    uint64_t flags = PTE_USER | PTE_NX | PTE_SHARED;
    if (protection & PROT_WRITE) flags |= PTE_WRITABLE;
    uint64_t next = start;
    for (; next < start + length; next += PAGE_SIZE) {
        uint64_t phys = pmm_alloc_page();
        if (!phys) break;
        if (vmm_map_page(process->address_space, next, phys, flags) != 0) {
            pmm_page_release(phys);
            break;
        }
    }
    if (next == start + length) return 0;
    while (next > start) {
        next -= PAGE_SIZE;
        uint64_t phys;
        if (vmm_get_mapping(process->address_space, next, &phys, NULL) == 0 &&
            vmm_unmap_page(process->address_space, next) == 0) pmm_page_release(phys);
    }
    return -STRATUM_ENOMEM;
}

int64_t user_map_anonymous(process_t *process, uint64_t length, unsigned protection, bool shared) {
    if (!process) return -STRATUM_ESRCH;
    length = page_length(length);
    if (!length || !valid_protection(protection)) return -STRATUM_EINVAL;

    user_region_t *free_slot = NULL;
    for (unsigned i = 0; i < PROCESS_ANON_REGIONS; i++) {
        if (!process->regions[i].length) {
            free_slot = &process->regions[i];
            break;
        }
    }
    if (!free_slot) return -STRATUM_ENOMEM;

    uint64_t candidate = ANON_BASE;
    while (length <= ANON_LIMIT - candidate) {
        uint64_t next = candidate;
        for (unsigned i = 0; i < PROCESS_ANON_REGIONS; i++) {
            user_region_t *r = &process->regions[i];
            if (r->length && candidate < r->start + r->length && r->start < candidate + length) {
                next = r->start + r->length;
                break;
            }
        }
        if (next == candidate) {
            if (shared) {
                int result = populate_shared(process, candidate, length, protection);
                if (result) return result;
            }
            *free_slot = (user_region_t){
                .start = candidate, .length = length, .protection = protection, .shared = shared,
            };
            return (int64_t)candidate;
        }
        candidate = next;
    }
    return -STRATUM_ENOMEM;
}

static user_region_t *whole_region(process_t *process, uint64_t start, uint64_t length) {
    if (!process) return NULL;
    user_region_t *region = find_region(process, start);
    return region && region->start == start && region->length == page_length(length) ? region : NULL;
}

int user_unmap(process_t *process, uint64_t start, uint64_t length) {
    user_region_t *region = whole_region(process, start, length);
    if (!region) return -STRATUM_EINVAL;
    futex_unmap(process, start, start + region->length);
    for (uint64_t va = start; va < start + region->length; va += PAGE_SIZE) {
        uint64_t phys;
        if (vmm_get_mapping(process->address_space, va, &phys, NULL) != 0) continue;
        if (vmm_unmap_page(process->address_space, va) != 0) return -STRATUM_EIO;
        pmm_page_release(phys);
    }
    *region = (user_region_t){0};
    return 0;
}

int user_protect(process_t *process, uint64_t start, uint64_t length, unsigned protection) {
    user_region_t *region = whole_region(process, start, length);
    if (!region || !valid_protection(protection)) return -STRATUM_EINVAL;
    for (uint64_t va = start; va < start + region->length; va += PAGE_SIZE) {
        uint64_t phys;
        if (vmm_get_mapping(process->address_space, va, &phys, NULL) != 0) continue;
        uint64_t flags = PTE_USER | PTE_NX;
        if (region->shared) flags |= PTE_SHARED;
        if (protection & PROT_WRITE) {
            bool cow = !region->shared && pmm_page_refcount(phys) > 1;
            flags |= cow ? PTE_COW : PTE_WRITABLE;
        }
        if (vmm_map_page(process->address_space, va, phys, flags) != 0) return -STRATUM_EIO;
    }
    region->protection = protection;
    return 0;
}

bool user_demand_page(process_t *process, uint64_t address, bool write, bool execute) {
    if (!process || execute) return false;
    user_region_t *region = find_region(process, address);
    if (!region || region->shared || (write && !(region->protection & PROT_WRITE))) return false;
    uint64_t phys = pmm_alloc_page();
    if (!phys) return false;
    uint64_t flags = PTE_USER | PTE_NX;
    if (region->protection & PROT_WRITE) flags |= PTE_WRITABLE;
    if (vmm_map_page(process->address_space, round_down(address, PAGE_SIZE), phys, flags) == 0) return true;
    pmm_free_page(phys);
    return false;
}
