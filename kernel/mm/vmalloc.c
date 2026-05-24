#include <kernel/mm/buddy.h>
#include <kernel/mm/heap.h>
#include <kernel/mm/pmm.h>
#include <kernel/sync.h>
#include <kernel/lib/string.h>
#include <asm/mmu.h>

/*
 * vmalloc: kernel virtual allocator.
 *
 * On Ranix (single-core, direct-mapped kernel), buddy pages are already
 * physically contiguous and accessible via PHYS_TO_VIRT — strictly better
 * than vmalloc semantics. We implement vmalloc as a buddy-backed allocator
 * with a size table so vfree can release correctly. Real non-contiguous-
 * physical mapping is deferred until SMP + IOMMU (v0.7+).
 */

#define VMALLOC_SLOTS 256

static struct {
    void   *ptr;
    int     order;
} vmalloc_table[VMALLOC_SLOTS];

static spinlock_t vmalloc_lock = SPINLOCK_INIT;

void *vmalloc(size_t size)
{
    if (size == 0) return NULL;
    size_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    int order = 0;
    while ((size_t)(1 << order) < pages) order++;

    void *phys = buddy_alloc(order);
    if (!phys) return NULL;
    void *va = (void *)PHYS_TO_VIRT(phys);
    memset(va, 0, (size_t)(1 << order) * PAGE_SIZE);

    uint64_t f = spin_lock_irq(&vmalloc_lock);
    for (int i = 0; i < VMALLOC_SLOTS; i++) {
        if (!vmalloc_table[i].ptr) {
            vmalloc_table[i].ptr   = va;
            vmalloc_table[i].order = order;
            spin_unlock_irq(&vmalloc_lock, f);
            return va;
        }
    }
    spin_unlock_irq(&vmalloc_lock, f);
    /* no slot — free and fail */
    buddy_free((uint64_t)phys, order);
    return NULL;
}

void vfree(void *ptr)
{
    if (!ptr) return;
    uint64_t f = spin_lock_irq(&vmalloc_lock);
    for (int i = 0; i < VMALLOC_SLOTS; i++) {
        if (vmalloc_table[i].ptr == ptr) {
            uint64_t phys = VIRT_TO_PHYS(ptr);
            int order = vmalloc_table[i].order;
            vmalloc_table[i].ptr   = NULL;
            vmalloc_table[i].order = 0;
            spin_unlock_irq(&vmalloc_lock, f);
            buddy_free(phys, order);
            return;
        }
    }
    spin_unlock_irq(&vmalloc_lock, f);
}

void *vzalloc(size_t size)
{
    return vmalloc(size); /* vmalloc already zeroes */
}
