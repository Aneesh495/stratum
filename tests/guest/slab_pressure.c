#include <kernel/kernel.h>
#include <kernel/pmm.h>
#include <kernel/slab.h>

void slab_pressure_run(void) {
    uint64_t pages_before, objects_before;
    slab_usage(&pages_before, &objects_before);
    uint64_t available = pmm_get_free_pages() + pages_before;
    static const size_t sizes[] = {16, 32, 64, 128, 256, 512, 1024, 2048};
    for (unsigned cls = 0; cls < sizeof(sizes) / sizeof(sizes[0]); cls++) {
        uint8_t *objects[32];
        for (unsigned i = 0; i < 32; i++) {
            objects[i] = kzalloc(sizes[cls]);
            kassert(objects[i] && !((uintptr_t)objects[i] & 15));
            for (size_t j = 0; j < sizes[cls]; j++) {
                kassert(objects[i][j] == 0);
                objects[i][j] = (uint8_t)(i + j);
            }
        }
        for (unsigned i = 0; i < 32; i++) {
            for (size_t j = 0; j < sizes[cls]; j++) kassert(objects[i][j] == (uint8_t)(i + j));
            kfree(objects[i]);
        }
    }

    void *full = kmalloc(2048);
    kassert(full);
    uint64_t head = 0;
    unsigned held = 0;
    for (uint64_t next; (next = pmm_alloc_page()) != 0;) {
        *(uint64_t *)phys_to_virt(next) = head;
        head = next;
        held++;
    }
    kassert(held > 256 && kmalloc(2048) == NULL);
    uint64_t released = head;
    head = *(uint64_t *)phys_to_virt(head);
    pmm_free_page(released);
    uint8_t *retry = kzalloc(2048);
    kassert(retry);
    for (size_t i = 0; i < 2048; i++) kassert(retry[i] == 0);
    kfree(retry);
    kfree(full);

    while (head) {
        uint64_t next = *(uint64_t *)phys_to_virt(head);
        pmm_free_page(head);
        head = next;
    }
    uint64_t pages_after, objects_after;
    slab_usage(&pages_after, &objects_after);
    kassert(objects_before == objects_after && available == pmm_get_free_pages() + pages_after);
    kprintf("[PASS] slab classes=8 objects=256 physical_pressure=%u recovered=%lu\n", held, available);
}
