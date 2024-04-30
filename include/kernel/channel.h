#ifndef STRATUM_KERNEL_CHANNEL_H
#define STRATUM_KERNEL_CHANNEL_H

#include <kernel/file.h>

int channel_create(file_t **first, file_t **second);
int64_t channel_read_user(file_t *file, void *buffer, size_t count);

#endif
