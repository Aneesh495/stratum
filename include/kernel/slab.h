#ifndef STRATUM_KERNEL_SLAB_H
#define STRATUM_KERNEL_SLAB_H

#include <kernel/types.h>

typedef struct kmem_cache kmem_cache_t;

void slab_init(void);
void slab_usage(uint64_t *pages, uint64_t *live_objects);

void *kmalloc(size_t size);
void *kzalloc(size_t size);
void  kfree(void *ptr);

kmem_cache_t *kmem_cache_create(const char *name, size_t obj_size, size_t align);
void         *kmem_cache_alloc(kmem_cache_t *cache);
void          kmem_cache_free(kmem_cache_t *cache, void *obj);

#endif /* STRATUM_KERNEL_SLAB_H */
