#ifndef STRATUM_KERNEL_BOOT_H
#define STRATUM_KERNEL_BOOT_H

#include <kernel/types.h>
#include <shared/boot_info.h>

extern boot_handoff_t g_boot_handoff;

int boot_check_header(const boot_handoff_t *handoff, uint64_t physical, uint64_t magic);
int boot_check_descriptors(const boot_handoff_t *handoff, uint64_t physical, const boot_mem_desc_t *entries);
int boot_validate_handoff(boot_handoff_t *handoff, uint64_t magic);
void boot_dump_info(const boot_handoff_t *handoff);

#endif /* STRATUM_KERNEL_BOOT_H */
