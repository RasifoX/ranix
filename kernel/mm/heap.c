#include <kernel/kprintf.h>
#include <kernel/mm/buddy.h>
#include <kernel/mm/heap.h>
#include <kernel/mm/mmu.h>
#include <kernel/mm/pmm.h>
#include <kernel/sync.h>
#include <asm/mmu.h>

#define HEAP_MAGIC      0x48454150
#define HEAP_LARGE      0x4C415247
#define SLAB_PAGE_MAGIC 0x534C4142

typedef struct slab_node {
    struct slab_node *next;
} slab_node_t;

typedef struct {
    uint32_t magic;
    uint32_t class_idx;
} heap_header_t;

/* Per-page slab metadata, stored in chunk 0 of each slab page. */
typedef struct {
    uint32_t page_magic;
    uint32_t class_idx;
    uint32_t free_count;   /* chunks currently on the free list */
    uint32_t total_count;  /* usable chunks per page (= N-1 slots) */
} slab_page_hdr_t;

#define NUM_CLASSES 7
static size_t size_classes[NUM_CLASSES] = { 32, 64, 128, 256, 512, 1024, 2048 };
static slab_node_t *free_lists[NUM_CLASSES];
static spinlock_t heap_lock = SPINLOCK_INIT;

static inline slab_page_hdr_t *page_hdr_of(void *ptr)
{
    return (slab_page_hdr_t *)((uint64_t)ptr & ~(uint64_t)(PAGE_SIZE - 1));
}

void heap_init(void) {
    for (int i = 0; i < NUM_CLASSES; i++)
        free_lists[i] = NULL;
}

void *kmalloc(size_t size) {
    if (size == 0) return NULL;

    size_t total = size + sizeof(heap_header_t);

    uint64_t f = spin_lock_irq(&heap_lock);

    /* Fast path: slab allocator for small objects */
    for (int i = 0; i < NUM_CLASSES; i++) {
        if (total > size_classes[i]) continue;

        if (free_lists[i]) {
            slab_node_t *node = free_lists[i];
            free_lists[i] = node->next;

            slab_page_hdr_t *phdr = page_hdr_of(node);
            if (phdr->page_magic == SLAB_PAGE_MAGIC)
                phdr->free_count--;

            spin_unlock_irq(&heap_lock, f);
            heap_header_t *hdr = (heap_header_t *)node;
            hdr->magic = HEAP_MAGIC;
            hdr->class_idx = (uint32_t)i;
            return (void *)(hdr + 1);
        }

        /* Allocate a new page; chunk 0 is the page header */
        void *page_phys = pmm_alloc();
        if (!page_phys) { spin_unlock_irq(&heap_lock, f); return NULL; }

        size_t chunk  = size_classes[i];
        size_t count  = PAGE_SIZE / chunk;   /* total slots */
        size_t usable = count - 1;           /* slots 1..N-1 are user data */

        uint8_t *p = (uint8_t *)PHYS_TO_VIRT(page_phys);

        slab_page_hdr_t *phdr = (slab_page_hdr_t *)p;
        phdr->page_magic  = SLAB_PAGE_MAGIC;
        phdr->class_idx   = (uint32_t)i;
        phdr->total_count = (uint32_t)usable;
        phdr->free_count  = (uint32_t)(usable - 1); /* 1 slot goes to caller */

        /* slot 1 → caller */
        heap_header_t *hdr = (heap_header_t *)(p + chunk);
        hdr->magic     = HEAP_MAGIC;
        hdr->class_idx = (uint32_t)i;

        /* slots 2..N-1 → free list */
        for (size_t j = 2; j < count; j++) {
            slab_node_t *node = (slab_node_t *)(p + j * chunk);
            node->next = free_lists[i];
            free_lists[i] = node;
        }

        spin_unlock_irq(&heap_lock, f);
        return (void *)(hdr + 1);
    }

    /* Slow path: buddy allocator for large objects */
    int ord = 0;
    while (((size_t)PAGE_SIZE << ord) < total) ord++;
    if (ord >= BUDDY_MAX_ORDER) { spin_unlock_irq(&heap_lock, f); return NULL; }

    void *page_phys = buddy_alloc(ord);
    if (!page_phys) { spin_unlock_irq(&heap_lock, f); return NULL; }

    heap_header_t *hdr = (heap_header_t *)PHYS_TO_VIRT(page_phys);
    hdr->magic     = HEAP_LARGE;
    hdr->class_idx = (uint32_t)ord;
    spin_unlock_irq(&heap_lock, f);
    return (void *)(hdr + 1);
}

void kfree(void *ptr) {
    if (!ptr) return;

    heap_header_t *hdr = (heap_header_t *)ptr - 1;
    uint64_t f = spin_lock_irq(&heap_lock);

    if (hdr->magic == HEAP_LARGE) {
        buddy_free(VIRT_TO_PHYS(hdr), hdr->class_idx);
    } else if (hdr->magic == HEAP_MAGIC) {
        uint32_t i = hdr->class_idx;
        if (i >= NUM_CLASSES) {
            kpanic("kfree: invalid SLAB class index");
        }

        /* Return chunk to free list */
        slab_node_t *node = (slab_node_t *)hdr;
        node->next = free_lists[i];
        free_lists[i] = node;

        /* Update per-page free count */
        slab_page_hdr_t *phdr = page_hdr_of(hdr);
        if (phdr->page_magic == SLAB_PAGE_MAGIC && phdr->class_idx == i) {
            phdr->free_count++;

            if (phdr->free_count == phdr->total_count) {
                /* All usable chunks are free — return page to PMM.
                 * Walk free list and remove every chunk belonging to this page. */
                uint64_t page_base = (uint64_t)phdr;
                slab_node_t **prev = &free_lists[i];
                while (*prev) {
                    uint64_t addr = (uint64_t)*prev;
                    if ((addr & ~(uint64_t)(PAGE_SIZE - 1)) == page_base)
                        *prev = (*prev)->next;
                    else
                        prev = &(*prev)->next;
                }
                spin_unlock_irq(&heap_lock, f);
                pmm_free((void *)VIRT_TO_PHYS(page_base));
                return;
            }
        }
    } else {
        kprintf("kfree: bad ptr=%p hdr=%p magic=0x%x\n", ptr, hdr, hdr->magic);
        kpanic("kfree: invalid pointer or corrupted header");
    }

    spin_unlock_irq(&heap_lock, f);
}
