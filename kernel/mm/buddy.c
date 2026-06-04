#include <kernel/mm/buddy.h>
#include <kernel/mm/pmm.h>
#include <kernel/mm/mmu.h>
#include <kernel/mm/heap.h>
#include <kernel/sync.h>
#include <kernel/hal.h>
#include <kernel/irq.h>
#include <kernel/smp.h>
#include <kernel/lib/string.h>
#include <kernel/kprintf.h>
#include <asm/mmu.h>

/* Free list is a doubly-linked list of page INDICES, with the next/prev links
 * stored in mem_map[] metadata — NOT in the free pages' own content. This makes
 * the allocator immune to a use-after-free *write* corrupting the free list
 * (which would otherwise hand the same page to two owners and cascade). */
#define BUDDY_NIL (~0ULL)

static uint64_t    free_list[BUDDY_MAX_ORDER]; /* page index of head, BUDDY_NIL = empty */
static uint8_t     *blk_order;
static uint8_t     *free_bm;
static uint64_t    _base;
static uint64_t    _npages;

static spinlock_t buddy_lock = SPINLOCK_INIT;
static uint64_t  _free_pages;

page_t *mem_map = NULL;

static void bm_set(uint64_t p) { free_bm[p >> 3] |=  (1 << (p & 7)); }
static void bm_clr(uint64_t p) { free_bm[p >> 3] &= ~(1 << (p & 7)); }
static int  bm_get(uint64_t p) { return  free_bm[p >> 3] &  (1 << (p & 7)); }

static uint64_t pidx(uint64_t addr) { return (addr - _base) >> PAGE_SHIFT; }
static uint64_t paddr(uint64_t p)   { return _base + (p << PAGE_SHIFT); }

/* Poison-on-free: stamp the first word of every freed page and verify it on the
 * next allocation. A free page has no legitimate writer, so a changed word means
 * a use-after-free *write* hit it while it sat on the free list. Per-page (not
 * per-block) so it survives buddy split/merge. Cheap: one word touched per page. */
#define BUDDY_POISON 0x6b6b6b6b6b6b6b6bULL

static void buddy_poison(uint64_t addr, int ord)
{
	uint64_t npg = 1ul << ord;
	for (uint64_t i = 0; i < npg; i++)
		*(volatile uint64_t *)PHYS_TO_VIRT(addr + (i << PAGE_SHIFT)) = BUDDY_POISON;
}

static int buddy_poison_ok(uint64_t addr, int ord)
{
	uint64_t npg = 1ul << ord;
	for (uint64_t i = 0; i < npg; i++)
		if (*(volatile uint64_t *)PHYS_TO_VIRT(addr + (i << PAGE_SHIFT)) != BUDDY_POISON)
			return 0;
	return 1;
}

page_t *paddr_to_page(uint64_t p)
{
	uint64_t idx = p >> PAGE_SHIFT;
	return (idx >= _npages) ? NULL : &mem_map[idx];
}

uint64_t page_to_paddr(page_t *page)
{
	if (!page) return 0;
	return (uint64_t)(page - mem_map) << PAGE_SHIFT;
}

/* free-list ops — operate on page indices, links live in mem_map[] */
static void push(int ord, uint64_t p)
{
	mem_map[p].free_prev = BUDDY_NIL;
	mem_map[p].free_next = free_list[ord];
	if (free_list[ord] != BUDDY_NIL)
		mem_map[free_list[ord]].free_prev = p;
	free_list[ord] = p;
}

static uint64_t pop(int ord)
{
	uint64_t p = free_list[ord];
	free_list[ord] = mem_map[p].free_next;
	if (free_list[ord] != BUDDY_NIL)
		mem_map[free_list[ord]].free_prev = BUDDY_NIL;
	return p;
}

static void list_remove(int ord, uint64_t p)
{
	uint64_t nx = mem_map[p].free_next, pv = mem_map[p].free_prev;
	if (pv != BUDDY_NIL) mem_map[pv].free_next = nx;
	else                 free_list[ord] = nx;
	if (nx != BUDDY_NIL) mem_map[nx].free_prev = pv;
}

/* Merge a block back into the buddy free list. Caller holds buddy_lock and has
 * already poisoned the block and run the double-free guard. */
static void buddy_free_locked(uint64_t addr, int ord)
{
	uint64_t p = pidx(addr);
	uint64_t npg = 1ul << ord;
	_free_pages += npg;
	for (uint64_t i = 0; i < npg; i++) {
		uint64_t idx = (addr + (i << PAGE_SHIFT)) >> PAGE_SHIFT;
		if (idx < _npages) mem_map[idx].refcount = 0;
	}

	while (ord < BUDDY_MAX_ORDER - 1) {
		uint64_t buddy = p ^ (1ul << ord);
		if (buddy >= _npages || !bm_get(buddy) || blk_order[buddy] != (uint8_t)ord)
			break;
		bm_clr(buddy);
		list_remove(ord, buddy);
		p = (p < buddy) ? p : buddy;
		ord++;
	}

	bm_set(p);
	blk_order[p] = (uint8_t)ord;
	push(ord, p);
}

/* Carve a block of `ord` off the free list (splitting larger if needed) and
 * return it allocated. Caller holds buddy_lock. NULL on OOM. */
static void *buddy_alloc_locked(int ord)
{
	int o = ord;
	while (o < BUDDY_MAX_ORDER && free_list[o] == BUDDY_NIL)
		o++;
	if (o >= BUDDY_MAX_ORDER) {
		kprintf("buddy: OOM (order %d)\n", ord);
		return NULL;
	}

	uint64_t p = pop(o);
	uint64_t addr = paddr(p);
	bm_clr(p);

	while (o-- > ord) {
		uint64_t half   = addr + ((uint64_t)PAGE_SIZE << o);
		uint64_t half_p = pidx(half);
		bm_set(half_p);
		blk_order[half_p] = (uint8_t)o;
		push(o, half_p);
	}

	blk_order[p] = (uint8_t)ord;

	if (!buddy_poison_ok(addr, ord))
		kprintf("buddy: USE-AFTER-FREE write detected on addr=%lx ord=%d (poison clobbered)\n", addr, ord);

	uint64_t npg = 1ul << ord;
	_free_pages -= npg;
	for (uint64_t i = 0; i < npg; i++) {
		uint64_t idx = (addr + (i << PAGE_SHIFT)) >> PAGE_SHIFT;
		if (idx < _npages) {
			mem_map[idx].refcount = 1;
			mem_map[idx].flags &= PG_MANAGED;
		}
	}
	return (void *)addr;
}

/*
 * Per-CPU order-0 magazine. The buddy_lock is the single global serialization
 * point for every page (de)allocation; under SMP the order-0 path (page
 * tables, COW pages, slab refills) dominates and would thrash that lock. Each
 * CPU keeps a small LIFO of order-0 pages so the common alloc/free hits no
 * global lock, draining/refilling in batches when it over/underflows.
 *
 * A magazined page is, to the rest of the buddy, an ordinary allocated block:
 * bm clear (invisible to merge), off every free list. Two invariants kept by
 * hand preserve the allocator's safety machinery on the fast path:
 *   - poison: parking poisons the page, taking it verifies the stamp — the same
 *     use-after-free-write detection a buddy round-trip gives.
 *   - double-free: PG_CACHED marks a parked page (global, so a cross-CPU double
 *     free is caught too); set on park, cleared on take.
 * Fast-path access runs with IRQs off so a nested IRQ alloc can't corrupt the
 * CPU's LIFO; no global lock is taken.
 */
#define MAG_DEPTH    62          /* fits the struct in one page per CPU */
#define MAG_REFILL   16          /* batch size for drain/refill under the lock */

typedef struct {
	uint32_t n;
	uint32_t pad;
	uint64_t pg[MAG_DEPTH];      /* page indices */
} buddy_mag_t;

static buddy_mag_t *buddy_mags;  /* [ncpus]; NULL until buddy_init_percpu */
static int          buddy_nmags;

void buddy_init_percpu(int ncpus)
{
	if (ncpus < 1) ncpus = 1;
	buddy_mag_t *m = kmalloc((size_t)ncpus * sizeof(buddy_mag_t));
	if (!m) return;                 /* stay on the locked path if we can't size it */
	memset(m, 0, (size_t)ncpus * sizeof(buddy_mag_t));
	buddy_nmags = ncpus;
	smp_wmb();
	buddy_mags = m;                 /* publish last: readers gate on non-NULL */
}

static buddy_mag_t *this_mag(void)
{
	if (!buddy_mags) return NULL;
	uint64_t id = arch_get_cpu_ctx()->cpu_id;
	if (id >= (uint64_t)buddy_nmags) return NULL;
	return &buddy_mags[id];
}

void buddy_free(uint64_t addr, int ord)
{
	if (ord == 0 && buddy_mags) {
		uint64_t fl = irq_save();
		buddy_mag_t *mag = this_mag();
		if (mag) {
			uint64_t p = pidx(addr);
			if (p < _npages && (mem_map[p].flags & PG_CACHED)) {
				irq_restore(fl);
				kprintf("buddy: DOUBLE-FREE addr=%lx ord=0 (cached)\n", addr);
				return;
			}
			if (mag->n == MAG_DEPTH) {
				/* overflow: drain a batch back to the buddy under the lock */
				uint64_t f = spin_lock_irq(&buddy_lock);
				for (int i = 0; i < MAG_REFILL; i++) {
					uint64_t dp = mag->pg[--mag->n];
					mem_map[dp].flags &= ~PG_CACHED;
					buddy_free_locked(paddr(dp), 0);
				}
				spin_unlock_irq(&buddy_lock, f);
			}
			if (p < _npages) { mem_map[p].flags |= PG_CACHED; mem_map[p].refcount = 0; }
			buddy_poison(addr, 0);
			mag->pg[mag->n++] = p;
			irq_restore(fl);
			return;
		}
		irq_restore(fl);
	}

	uint64_t f = spin_lock_irq(&buddy_lock);
	uint64_t p = pidx(addr);
	/* Double-free guard: a block already on the free list at this order would
	 * be pushed twice → the same page handed to two allocators. Catch + refuse. */
	if (p < _npages && bm_get(p) && blk_order[p] == (uint8_t)ord) {
		spin_unlock_irq(&buddy_lock, f);
		kprintf("buddy: DOUBLE-FREE addr=%lx ord=%d (already free)\n", addr, ord);
		return;
	}
	buddy_poison(addr, ord);
	buddy_free_locked(addr, ord);
	spin_unlock_irq(&buddy_lock, f);
}

void *buddy_alloc(int ord)
{
	if (ord == 0 && buddy_mags) {
		uint64_t fl = irq_save();
		buddy_mag_t *mag = this_mag();
		if (mag) {
			if (mag->n == 0) {
				/* underflow: refill a batch from the buddy under the lock.
				 * buddy_alloc_locked leaves the block poisoned (it only checks
				 * the stamp), so a parked page stays poisoned for the pop check. */
				uint64_t f = spin_lock_irq(&buddy_lock);
				for (int i = 0; i < MAG_REFILL; i++) {
					void *q = buddy_alloc_locked(0);
					if (!q) break;
					uint64_t qp = pidx((uint64_t)q);
					mem_map[qp].flags |= PG_CACHED;
					mag->pg[mag->n++] = qp;
				}
				spin_unlock_irq(&buddy_lock, f);
			}
			if (mag->n) {
				uint64_t p = mag->pg[--mag->n];
				uint64_t addr = paddr(p);
				if (p < _npages) {
					mem_map[p].flags &= PG_MANAGED;   /* clears PG_CACHED */
					mem_map[p].refcount = 1;
				}
				int bad = !buddy_poison_ok(addr, 0);
				irq_restore(fl);
				if (bad)
					kprintf("buddy: USE-AFTER-FREE write detected on addr=%lx ord=0 (poison clobbered)\n", addr);
				return (void *)addr;
			}
		}
		irq_restore(fl);
	}

	uint64_t f = spin_lock_irq(&buddy_lock);
	void *r = buddy_alloc_locked(ord);
	if (!r && buddy_mags) {
		/* Pages may be stranded in this CPU's magazine — flush it back and retry
		 * once before declaring OOM. */
		buddy_mag_t *mag = this_mag();
		if (mag) {
			while (mag->n) {
				uint64_t dp = mag->pg[--mag->n];
				mem_map[dp].flags &= ~PG_CACHED;
				buddy_free_locked(paddr(dp), 0);
			}
			r = buddy_alloc_locked(ord);
		}
	}
	spin_unlock_irq(&buddy_lock, f);
	return r;
}

void buddy_add_region(uint64_t addr, uint64_t size)
{
	uint64_t end = (addr + size) & ~(uint64_t)(PAGE_SIZE - 1);
	addr = (addr + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);

	for (uint64_t a = addr; a < end; a += PAGE_SIZE) {
		uint64_t idx = pidx(a);
		if (idx < _npages) mem_map[idx].flags |= PG_MANAGED;
	}

	while (addr < end) {
		int o = BUDDY_MAX_ORDER - 1;
		while (o > 0) {
			uint64_t blk = (uint64_t)PAGE_SIZE << o;
			if ((addr & (blk - 1)) == 0 && addr + blk <= end)
				break;
			o--;
		}
		buddy_free(addr, o);
		addr += (uint64_t)PAGE_SIZE << o;
	}
}

uint64_t buddy_init(uint64_t base, uint64_t limit)
{
	uint64_t total_pages = limit >> PAGE_SHIFT;

	uint64_t blk_order_sz = total_pages * sizeof(uint8_t);
	uint64_t free_bm_sz   = (total_pages + 7) / 8;
	uint64_t mem_map_sz   = total_pages * sizeof(page_t);

	uint64_t meta_sz    = blk_order_sz + free_bm_sz + mem_map_sz;
	uint64_t meta_pages = (meta_sz + PAGE_SIZE - 1) >> PAGE_SHIFT;

	if (meta_pages >= total_pages) return 0;

	blk_order = (uint8_t *)PHYS_TO_VIRT(base);
	free_bm   = (uint8_t *)PHYS_TO_VIRT(base + blk_order_sz);
	mem_map   = (page_t *)PHYS_TO_VIRT(base + blk_order_sz + free_bm_sz);

	_base       = 0;
	_npages     = total_pages;
	_free_pages = 0;

	for (int i = 0; i < BUDDY_MAX_ORDER; i++) free_list[i] = BUDDY_NIL;
	memset(blk_order, 0, blk_order_sz);
	memset(free_bm,   0, free_bm_sz);
	memset(mem_map,   0, mem_map_sz);

	return meta_pages << PAGE_SHIFT;
}

uint64_t buddy_free_pages(void)
{
	uint64_t f = spin_lock_irq(&buddy_lock);
	uint64_t n = _free_pages;
	spin_unlock_irq(&buddy_lock, f);
	return n;
}

uint64_t buddy_total_pages(void) { return _npages; }
