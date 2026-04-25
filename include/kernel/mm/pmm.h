#ifndef PMM_H
#define PMM_H

#include <uapi/types.h>
#include <kernel/mm/mmu.h>

#ifndef PAGE_SIZE
#define PAGE_SIZE 4096
#endif

uint64_t pmm_init(uint64_t mem_start, uint64_t mem_limit);
void pmm_free_region(uint64_t addr, uint64_t size);
void *pmm_alloc(void);
void pmm_free(void *page);

uint64_t pmm_free_pages(void);

#endif
