#ifndef PMM_H
#define PMM_H

#include <uapi/types.h>
#include <kernel/mm/mmu.h>

#ifndef PAGE_SIZE
#define PAGE_SIZE 4096
#endif

typedef struct page {
	uint32_t refcount;
	uint32_t flags;
	uint64_t free_next;   /* buddy free-list link (page index; valid only while free) */
	uint64_t free_prev;
} page_t;

#define PG_MANAGED  (1 << 0)
#define PG_CACHED   (1 << 1)   /* order-0 page parked in a per-CPU buddy magazine */

extern page_t *mem_map;

uint64_t pmm_init(uint64_t mem_start, uint64_t mem_limit);
void pmm_free_region(uint64_t addr, uint64_t size);
void *pmm_alloc(void);
int  pmm_free(void *page);   /* 1 if that was the last reference */
void pmm_hold(void *page);
long pmm_free_block(uint64_t phys, int order);

page_t *paddr_to_page(uint64_t paddr);
uint64_t page_to_paddr(page_t *page);

uint64_t pmm_free_pages(void);

#endif
