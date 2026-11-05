#include <kernel/mm/pmm.h>
#include <kernel/mm/buddy.h>
#include <kernel/sync.h>
#include <kernel/kprintf.h>

uint64_t pmm_init(uint64_t base, uint64_t limit) { return buddy_init(base, limit); }
void     pmm_free_region(uint64_t addr, uint64_t size) { buddy_add_region(addr, size); }
void    *pmm_alloc(void)   { return buddy_alloc(0); }

static spinlock_t pmm_lock = SPINLOCK_INIT;

void pmm_hold(void *p)
{
	page_t *pg = paddr_to_page((uint64_t)p);
	if (!pg || !(pg->flags & PG_MANAGED)) return;
	uint64_t f = spin_lock_irq(&pmm_lock);
	pg->refcount++;
	spin_unlock_irq(&pmm_lock, f);
}

int pmm_free(void *p)
{
	page_t *pg = paddr_to_page((uint64_t)p);
	if (!pg || !(pg->flags & PG_MANAGED)) return 0;

	uint64_t f = spin_lock_irq(&pmm_lock);
	if (pg->refcount == 0) {
		spin_unlock_irq(&pmm_lock, f);
		kprintf(KERN_ERR "mm: pmm_free double free phys=%p\n", p);
		return 0;
	}
	pg->refcount--;
	if (pg->refcount == 0) {
		spin_unlock_irq(&pmm_lock, f);
		buddy_free((uint64_t)p, 0);
		return 1;
	}
	spin_unlock_irq(&pmm_lock, f);
	return 0;
}

/* Release a buddy block; returns the pages actually freed. With a page still
 * mapped it goes page by page, so that page stays until the mapping lets go of it;
 * otherwise whole, which keeps it out of the per-CPU caches, where its pages would
 * stop the block from merging. */
long pmm_free_block(uint64_t phys, int order)
{
	uint64_t n = 1ULL << order, f = spin_lock_irq(&pmm_lock);
	int sole = 1;
	for (uint64_t i = 0; i < n && sole; i++)
		sole = paddr_to_page(phys + i * PAGE_SIZE)->refcount == 1;
	spin_unlock_irq(&pmm_lock, f);
	if (sole) { buddy_free(phys, order); return (long)n; }
	long freed = 0;
	for (uint64_t i = 0; i < n; i++)
		freed += pmm_free((void *)(phys + i * PAGE_SIZE));
	return freed;
}

uint64_t pmm_free_pages(void) { return buddy_free_pages(); }
