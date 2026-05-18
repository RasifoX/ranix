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

void pmm_free(void *p)
{
	page_t *pg = paddr_to_page((uint64_t)p);
	if (!pg || !(pg->flags & PG_MANAGED)) return;

	uint64_t f = spin_lock_irq(&pmm_lock);
	if (pg->refcount == 0) {
		spin_unlock_irq(&pmm_lock, f);
		kprintf("pmm_free: double-free phys=%p\n", p);
		return;
	}
	pg->refcount--;
	if (pg->refcount == 0) {
		spin_unlock_irq(&pmm_lock, f);
		buddy_free((uint64_t)p, 0);
		return;
	}
	spin_unlock_irq(&pmm_lock, f);
}

uint64_t pmm_free_pages(void) { return buddy_free_pages(); }
