#include <kernel/slab.h>
#include <kernel/pmm.h>
#include <kernel/spinlock.h>
#include <kernel/kernel.h>
#include <kernel/string.h>

#define SLAB_MAGIC 0x534C4142 /* "SLAB" */
#define LARG_MAGIC 0x4C415247 /* "LARG" */

#define NUM_SIZE_CLASSES 8
static const size_t g_size_classes[NUM_SIZE_CLASSES] = { 16, 32, 64, 128, 256, 512, 1024, 2048 };

typedef struct slab_page {
    uint32_t magic;
    kmem_cache_t *cache;
    void *free_list;
    uint32_t inuse;
    uint32_t capacity;
    struct slab_page *next;
    uint64_t allocated[8];
} slab_page_t;

struct kmem_cache {
    char name[32];
    size_t obj_size;
    size_t align;
    slab_page_t *slabs_partial;
    slab_page_t *slabs_full;
    uint64_t total_allocs;
    uint64_t total_frees;
    spinlock_t lock;
};

static kmem_cache_t g_general_caches[NUM_SIZE_CLASSES];

typedef struct {
    uint32_t magic;
    uint32_t order;
    size_t size;
} large_alloc_header_t;

void slab_init(void) {
    for (int i = 0; i < NUM_SIZE_CLASSES; i++) {
        kmem_cache_t *c = &g_general_caches[i];
        snprintf(c->name, sizeof(c->name), "kmalloc-%zu", g_size_classes[i]);
        c->obj_size = g_size_classes[i];
        c->align = (g_size_classes[i] >= 16) ? 16 : 8;
        c->slabs_partial = NULL;
        c->slabs_full = NULL;
        c->total_allocs = 0;
        c->total_frees = 0;
        spin_lock_init(&c->lock);
    }
    kprintf("[SLAB] Object cache allocator initialized with %d size classes (16B - 2048B)\n", NUM_SIZE_CLASSES);
}

kmem_cache_t *kmem_cache_create(const char *name, size_t obj_size, size_t align) {
    if (align < sizeof(void *)) align = sizeof(void *);
    if (!obj_size || obj_size > PAGE_SIZE || align >= PAGE_SIZE || (align & (align - 1))) return NULL;
    size_t size = round_up(obj_size, align);
    if (round_up(sizeof(slab_page_t), align) + size > PAGE_SIZE) return NULL;

    /* Allocate cache descriptor from general cache */
    kmem_cache_t *c = (kmem_cache_t *)kmalloc(sizeof(kmem_cache_t));
    if (!c) return NULL;

    strncpy(c->name, name ? name : "custom", sizeof(c->name) - 1);
    c->name[sizeof(c->name) - 1] = '\0';
    c->obj_size = size;
    c->align = align;
    c->slabs_partial = NULL;
    c->slabs_full = NULL;
    c->total_allocs = 0;
    c->total_frees = 0;
    spin_lock_init(&c->lock);

    return c;
}

static slab_page_t *alloc_slab_page(kmem_cache_t *cache) {
    uint64_t paddr = pmm_alloc_page();
    if (!paddr) return NULL;

    slab_page_t *slab = (slab_page_t *)phys_to_virt(paddr);
    slab->magic = SLAB_MAGIC;
    slab->cache = cache;
    slab->inuse = 0;
    slab->next = NULL;

    /* Build linked free list in remaining page space */
    size_t offset = (sizeof(slab_page_t) + cache->align - 1) & ~(cache->align - 1);
    slab->free_list = (void *)((uint8_t *)slab + offset);

    uint32_t count = 0;
    uint8_t *curr = (uint8_t *)slab->free_list;
    while (offset + cache->obj_size <= PAGE_SIZE) {
        uint8_t *next = curr + cache->obj_size;
        if (offset + 2 * cache->obj_size <= PAGE_SIZE) {
            *(void **)curr = (void *)next;
        } else {
            *(void **)curr = NULL;
        }
        curr = next;
        offset += cache->obj_size;
        count++;
    }

    slab->capacity = count;
    return slab;
}

void *kmem_cache_alloc(kmem_cache_t *cache) {
    if (!cache) return NULL;

    uint64_t flags;
    spin_lock_irqsave(&cache->lock, &flags);
    slab_page_t *slab = cache->slabs_partial;
    if (!slab) {
        slab = alloc_slab_page(cache);
        if (!slab) {
            spin_unlock_irqrestore(&cache->lock, flags);
            return NULL;
        }

        slab->next = cache->slabs_partial;
        cache->slabs_partial = slab;
    }

    void *obj = slab->free_list;
    slab->free_list = *(void **)obj;
    size_t offset = round_up(sizeof(*slab), cache->align);
    size_t index = ((uint8_t *)obj - (uint8_t *)slab - offset) / cache->obj_size;
    slab->allocated[index / 64] |= 1ULL << (index % 64);
    slab->inuse++;
    cache->total_allocs++;

    /* If slab is now full, move to full list */
    if (slab->inuse == slab->capacity) {
        cache->slabs_partial = slab->next;
        slab->next = cache->slabs_full;
        cache->slabs_full = slab;
    }

    spin_unlock_irqrestore(&cache->lock, flags);
    return obj;
}

void kmem_cache_free(kmem_cache_t *cache, void *obj) {
    if (!cache || !obj) return;

    /* Find slab header at page base */
    uint64_t page_base = ((uint64_t)obj) & ~(PAGE_SIZE - 1);
    slab_page_t *slab = (slab_page_t *)page_base;

    uint64_t flags;
    spin_lock_irqsave(&cache->lock, &flags);
    size_t start = round_up(sizeof(*slab), cache->align);
    size_t offset = (uint64_t)obj - page_base;
    size_t index = offset >= start ? (offset - start) / cache->obj_size : slab->capacity;
    if (slab->magic != SLAB_MAGIC || slab->cache != cache || index >= slab->capacity ||
        (offset - start) % cache->obj_size || !(slab->allocated[index / 64] & (1ULL << (index % 64)))) {
        spin_unlock_irqrestore(&cache->lock, flags);
        kprintf("[SLAB] Invalid object release at %p.\n", obj);
        return;
    }

    slab->allocated[index / 64] &= ~(1ULL << (index % 64));
    *(void **)obj = slab->free_list;
    slab->free_list = obj;
    slab->inuse--;
    cache->total_frees++;

    if (slab->inuse + 1 == slab->capacity) {
        slab_page_t **slot = &cache->slabs_full;
        while (*slot != slab) slot = &(*slot)->next;
        *slot = slab->next;
        slab->next = cache->slabs_partial;
        cache->slabs_partial = slab;
    }
    if (slab->inuse == 0 && cache->slabs_partial->next) {
        slab_page_t **slot = &cache->slabs_partial;
        while (*slot != slab) slot = &(*slot)->next;
        *slot = slab->next;
        slab->magic = 0;
        pmm_free_page(virt_to_phys(slab));
    }
    spin_unlock_irqrestore(&cache->lock, flags);
}

void *kmalloc(size_t size) {
    if (size == 0) return NULL;

    /* Small allocation from size classes */
    for (int i = 0; i < NUM_SIZE_CLASSES; i++) {
        if (size <= g_size_classes[i]) {
            return kmem_cache_alloc(&g_general_caches[i]);
        }
    }

    /* Large allocation directly from PMM */
    if (size > (size_t)-1 - sizeof(large_alloc_header_t) - PAGE_SIZE) return NULL;
    size_t total_size = size + sizeof(large_alloc_header_t);
    size_t pages = (total_size + PAGE_SIZE - 1) / PAGE_SIZE;

    uint32_t order = 0;
    while ((1ULL << order) < pages) {
        order++;
    }

    uint64_t paddr = pmm_alloc_pages(order);
    if (!paddr) return NULL;

    large_alloc_header_t *hdr = (large_alloc_header_t *)phys_to_virt(paddr);
    hdr->magic = LARG_MAGIC;
    hdr->order = order;
    hdr->size = size;

    return (void *)(hdr + 1);
}

void *kzalloc(size_t size) {
    void *ptr = kmalloc(size);
    if (ptr) {
        memset(ptr, 0, size);
    }
    return ptr;
}

void kfree(void *ptr) {
    if (!ptr) return;

    /* Check if object is from a small slab */
    uint64_t page_base = ((uint64_t)ptr) & ~(PAGE_SIZE - 1);
    slab_page_t *slab = (slab_page_t *)page_base;

    if (slab->magic == SLAB_MAGIC && slab->cache != NULL) {
        kmem_cache_free(slab->cache, ptr);
        return;
    }

    /* Check if large allocation */
    large_alloc_header_t *hdr = ((large_alloc_header_t *)ptr) - 1;
    if (hdr->magic == LARG_MAGIC) {
        uint32_t order = hdr->order;
        hdr->magic = 0; /* Clear magic to prevent double free */
        uint64_t paddr = virt_to_phys(hdr);
        pmm_free_pages(paddr, order);
        return;
    }

    kprintf("[SLAB] ERROR: kfree received unmanaged pointer %p\n", ptr);
}
