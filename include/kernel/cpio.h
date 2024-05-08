#ifndef STRATUM_KERNEL_CPIO_H
#define STRATUM_KERNEL_CPIO_H

#include <kernel/types.h>

typedef struct {
    const char *name;
    const uint8_t *data;
    uint32_t size;
    uint32_t mode;
} cpio_file_t;

int cpio_scan_bin(const uint8_t *archive, size_t size, cpio_file_t *files, unsigned capacity,
                  unsigned *file_count);

#endif
