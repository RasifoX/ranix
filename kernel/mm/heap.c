#include <kernel/kprintf.h>
#include <kernel/mm/buddy.h>
#include <kernel/mm/heap.h>
#include <kernel/mm/mmu.h>
#include <kernel/mm/pmm.h>
#include <kernel/sync.h>

#define HEAP_MAGIC 0x48454150
#define HEAP_LARGE 0x4C415247

typedef struct slab_node {
    struct slab_node *next;
} slab_node_t;

typedef struct {
    uint32_t magic;
    uint32_t class_idx;
} heap_header_t;

#define NUM_CLASSES 7
static size_t size_classes[NUM_CLASSES] = { 32, 64, 128, 256, 512, 1024, 2048 };
static slab_node_t *free_lists[NUM_CLASSES];
static spinlock_t heap_lock = SPINLOCK_INIT;

void heap_init(void) {
    for (int i = 0; i < NUM_CLASSES; i++) {
        free_lists[i] = NULL;
    }
}

void *kmalloc(size_t size) {
    if (size == 0) return NULL;
    
    size_t total = size + sizeof(heap_header_t);
    
    uint64_t f = spin_lock_irq(&heap_lock);
    
    /* Fast path: SLAB allocator for small objects */
    for (int i = 0; i < NUM_CLASSES; i++) {
        if (total <= size_classes[i]) {
            if (free_lists[i]) {
                slab_node_t *node = free_lists[i];
                free_lists[i] = node->next;
                spin_unlock_irq(&heap_lock, f);
                
                heap_header_t *hdr = (heap_header_t *)node;
                hdr->magic = HEAP_MAGIC;
                hdr->class_idx = i;
                return (void *)(hdr + 1);
            }
            
            /* Allocate a new page and slice it */
            void *page = pmm_alloc();
            if (!page) {
                spin_unlock_irq(&heap_lock, f);
                return NULL;
            }
            
            size_t chunk = size_classes[i];
            size_t count = PAGE_SIZE / chunk;
            
            uint8_t *p = (uint8_t *)page;
            /* first chunk goes to the user */
            heap_header_t *hdr = (heap_header_t *)p;
            hdr->magic = HEAP_MAGIC;
            hdr->class_idx = i;
            
            /* remaining chunks go to free list */
            for (size_t j = 1; j < count; j++) {
                slab_node_t *node = (slab_node_t *)(p + j * chunk);
                node->next = free_lists[i];
                free_lists[i] = node;
            }
            
            spin_unlock_irq(&heap_lock, f);
            return (void *)(hdr + 1);
        }
    }
    
    /* Slow path: Buddy allocator for large objects */
    int ord = 0;
    while (((size_t)PAGE_SIZE << ord) < total) ord++;
    if (ord >= BUDDY_MAX_ORDER) {
        spin_unlock_irq(&heap_lock, f);
        return NULL;
    }
    
    void *page = buddy_alloc(ord);
    if (!page) {
        spin_unlock_irq(&heap_lock, f);
        return NULL;
    }
    
    heap_header_t *hdr = (heap_header_t *)page;
    hdr->magic = HEAP_LARGE;
    hdr->class_idx = ord;
    spin_unlock_irq(&heap_lock, f);
    return (void *)(hdr + 1);
}

void kfree(void *ptr) {
    if (!ptr) return;
    
    heap_header_t *hdr = (heap_header_t *)ptr - 1;
    
    uint64_t f = spin_lock_irq(&heap_lock);
    
    if (hdr->magic == HEAP_LARGE) {
        buddy_free((uint64_t)hdr, hdr->class_idx);
    } else if (hdr->magic == HEAP_MAGIC) {
        uint32_t i = hdr->class_idx;
        if (i < NUM_CLASSES) {
            slab_node_t *node = (slab_node_t *)hdr;
            node->next = free_lists[i];
            free_lists[i] = node;
        } else {
            kpanic("kfree: invalid SLAB class index");
        }
    } else {
        kpanic("kfree: invalid pointer or corrupted header");
    }
    
    spin_unlock_irq(&heap_lock, f);
}
