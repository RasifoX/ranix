#include <kernel/mm/pmm.h>
#include <kernel/mm/buddy.h>

uint64_t pmm_init(uint64_t base, uint64_t limit) { return buddy_init(base, limit); }
void     pmm_free_region(uint64_t addr, uint64_t size) { buddy_add_region(addr, size); }
void    *pmm_alloc(void)   { return buddy_alloc(0); }
void pmm_hold(void *p)
{
	page_t *pg = paddr_to_page((uint64_t)p);
	if (pg && (pg->flags & PG_MANAGED)) pg->refcount++;
}

void pmm_free(void *p)
{
	page_t *pg = paddr_to_page((uint64_t)p);
	if (!pg || !(pg->flags & PG_MANAGED)) return;

	if (pg->refcount > 0) {
		pg->refcount--;
		if (pg->refcount == 0) {
			buddy_free((uint64_t)p, 0);
		}
	}
}
uint64_t pmm_free_pages(void) { return buddy_free_pages(); }
