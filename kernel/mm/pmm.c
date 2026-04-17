#include <kernel/mm/pmm.h>
#include <kernel/mm/buddy.h>

void     pmm_init(uint64_t base, uint64_t size) { buddy_init(base, size); }
void     pmm_free_region(uint64_t addr, uint64_t size) { buddy_add_region(addr, size); }
void    *pmm_alloc(void)   { return buddy_alloc(0); }
void     pmm_free(void *p) { buddy_free((uint64_t)p, 0); }
uint64_t pmm_free_pages(void) { return buddy_free_pages(); }
