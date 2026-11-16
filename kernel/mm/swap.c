/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* Swap core: slots, the swap cache, swap-in, and swap entries in page tables.
 *
 * A page goes out in two steps. Reclaim turns each page-table entry that maps
 * it into a swap entry (swap_out_pte): the page enters the swap cache with a
 * slot, and the entry's reference on the page becomes a reference on the slot.
 * Once only the cache holds the page, it is written to its slot and freed
 * (swap_writeback). Coming back (swap_fault) the page is taken from the cache
 * when it is still there - a fork relative shares the slot - else read into a
 * new page that enters the cache, so a slot is read once however many page
 * tables point at it. When no page table points at a slot any more, the slot
 * and its cache entry go.
 *
 * A swapped page stays charged to its cgroup, as Linux counts swap in the OOM
 * score; the charge goes when the last reference to the page, in RAM or on
 * swap, goes. Locks: vs->fault_lock, then swap_lock, then pmm_lock. No I/O
 * is done under a spinlock. Design: arastirma/swap.md. */
#include "swap_area.h"
#include <kernel/psi.h>
#include <kernel/mm/pmm.h>
#include <kernel/mm/buddy.h>
#include <kernel/mm/oom.h>
#include <kernel/mm/mmu.h>
#include <kernel/proc.h>
#include <kernel/errno.h>
#include <kernel/lib/string.h>
#include <kernel/cgroup.h>
#include <kernel/irq.h>
#include <asm/mmu.h>

swap_area_t  swap_areas[SWAP_AREAS];
spinlock_t   swap_lock = SPINLOCK_INIT;
wait_queue_t swap_wait = WAIT_QUEUE_INIT;
uint64_t     swap_nr_in, swap_nr_out;
uint64_t     swap_nr_cached;

/* The area of a swap entry, NULL if no area is on there or the slot is out of it. */
static swap_area_t *area_of(uint64_t e)
{
	unsigned i = swp_area(e);
	if (i >= SWAP_AREAS) return NULL;
	swap_area_t *a = &swap_areas[i];
	uint64_t slot = swp_slot(e);
	if (!a->count || slot == 0 || slot >= a->pages) return NULL;
	return a;
}

static uint64_t slot_lba(swap_area_t *a, uint64_t slot)
{
	return a->lba ? a->lba[slot] : slot * (PAGE_SIZE / BLOCK_SIZE);
}

int swap_io(swap_area_t *a, uint64_t slot, void *buf, int write)
{
	uint64_t lba = slot_lba(a, slot);
	int r = write ? block_write(a->bdev, lba, buf, PAGE_SIZE / BLOCK_SIZE)
	              : block_read(a->bdev, lba, buf, PAGE_SIZE / BLOCK_SIZE);
	return r ? -EIO : 0;
}

/* Under swap_lock: one reference on the slot goes; at none the slot is free. */
static void slot_put(swap_area_t *a, uint64_t slot)
{
	if (a->count[slot] == 0) return;
	if (--a->count[slot] == 0) {
		a->inuse--;
		if (zram_owns(a->bdev)) zram_discard(a->bdev, slot_lba(a, slot));
	}
}

/* Under swap_lock: a free slot with the cache's reference, as a swap entry. */
static int slot_alloc(uint64_t *e)
{
	swap_area_t *best = NULL;
	for (int i = 0; i < SWAP_AREAS; i++) {
		swap_area_t *a = &swap_areas[i];
		if (!a->on || a->draining || a->inuse + 1 >= a->pages) continue;
		if (!best || a->prio > best->prio) best = a;
	}
	if (!best) return -1;
	uint64_t s = best->next;
	for (uint64_t n = 1; n < best->pages; n++, s++) {
		if (s == 0 || s >= best->pages) s = 1;
		if (best->count[s] == 0) {
			best->count[s] = 1;
			best->inuse++;
			best->next = s + 1;
			*e = swp_make((unsigned)(best - swap_areas), s);
			return 0;
		}
	}
	return -1;
}

/* Under swap_lock: the page leaves the cache; the caller drops the cache's
 * page reference (pmm_free) itself. */
static void cache_drop(swap_area_t *a, uint64_t slot, page_t *pg)
{
	a->cache[slot] = 0;
	swap_nr_cached--;
	pg->flags &= ~(uint32_t)PG_SWAPCACHE;
	pg->swap = 0;
	slot_put(a, slot);
}

/* Under swap_lock: if nothing but the cache points at the slot, the cache
 * entry is useless; drop it. 1 when that freed the page. */
static int cache_trim(swap_area_t *a, uint64_t slot)
{
	uint64_t pa = a->cache[slot];
	if (!pa || a->count[slot] != 1) return 0;
	page_t *pg = paddr_to_page(pa);
	if (pg->flags & (PG_SWAPREAD | PG_SWAPWRITE)) return 0;   /* the I/O's end trims it */
	cache_drop(a, slot, pg);
	return pmm_free((void *)pa);
}

void swap_pte_dup(uint64_t e)
{
	uint64_t f = spin_lock_irq(&swap_lock);
	swap_area_t *a = area_of(e);
	if (a && a->count[swp_slot(e)]) a->count[swp_slot(e)]++;
	spin_unlock_irq(&swap_lock, f);
}

int swap_pte_free(uint64_t e)
{
	int gone = 0;
	uint64_t f = spin_lock_irq(&swap_lock);
	swap_area_t *a = area_of(e);
	uint64_t slot = swp_slot(e);
	if (a && a->count[slot]) {
		slot_put(a, slot);
		uint64_t pa = a->cache[slot];
		if (a->count[slot] == 0) gone = 1;
		else if (pa && a->count[slot] == 1)      /* only the cache is left */
			gone = paddr_to_page(pa)->refcount == 1;
		cache_trim(a, slot);
	}
	spin_unlock_irq(&swap_lock, f);
	return gone;
}

/* Cache pages only the cache holds now, while other page tables still point at
 * their slot: written and freed by the next reclaim (swap_writeback_pending). */
#define PENDING 64
static uint64_t pending[PENDING];
static unsigned npending;

int anon_page_put(uint64_t pa)
{
	page_t *pg = paddr_to_page(pa);
	if (!pg || !(pg->flags & PG_SWAPCACHE)) return pmm_free((void *)pa);
	int gone = 0;
	uint64_t f = spin_lock_irq(&swap_lock);
	if (pmm_free((void *)pa)) {
		gone = 1;                        /* left the cache meanwhile */
	} else if (pg->flags & PG_SWAPCACHE) {
		swap_area_t *a = area_of(pg->swap);
		uint64_t slot = swp_slot(pg->swap);
		if (a && pg->refcount == 1 && a->count[slot] == 1) {
			gone = 1;                    /* nothing points at it: the cache goes too */
			cache_trim(a, slot);
		} else if (a && pg->refcount == 1 && npending < PENDING) {
			pending[npending++] = pa;    /* out already elsewhere: write it, free the RAM */
		}
	}
	spin_unlock_irq(&swap_lock, f);
	return gone;
}

long swap_writeback_pending(void)
{
	uint64_t take[PENDING];
	uint64_t f = spin_lock_irq(&swap_lock);
	unsigned n = npending;
	for (unsigned i = 0; i < n; i++) take[i] = pending[i];
	npending = 0;
	spin_unlock_irq(&swap_lock, f);
	return swap_writeback(take, (int)n);   /* writeback_one checks each page again */
}

void anon_page_mark(uint64_t pa)
{
	page_t *pg = paddr_to_page(pa);
	if (pg && (pg->flags & PG_MANAGED)) atomic_or32(&pg->flags, PG_ANON);
}

int swap_out_pte(vm_space_t *vs, uint64_t va, uint64_t *pte, uint64_t *wb)
{
	uint64_t pa = arch_pte_page(*pte);
	page_t *pg = pa ? paddr_to_page(pa) : NULL;
	*wb = 0;
	if (!pg || !(pg->flags & PG_ANON) || (pg->flags & PG_SWAPREAD)) return 0;
	uint64_t f = spin_lock_irq(&swap_lock);
	if (!(pg->flags & PG_SWAPCACHE)) {
		uint64_t e;
		if (slot_alloc(&e) != 0) { spin_unlock_irq(&swap_lock, f); return -1; }
		area_of(e)->cache[swp_slot(e)] = pa;
		swap_nr_cached++;
		pg->swap = e;
		pg->flags |= PG_SWAPCACHE;
		pmm_hold((void *)pa);                 /* the cache's reference */
	}
	uint64_t e = pg->swap;
	swap_area_t *a = area_of(e);
	if (!a || a->draining) { spin_unlock_irq(&swap_lock, f); return 0; }   /* swapoff is emptying it */
	a->count[swp_slot(e)]++;
	arch_pte_set(vs, va, pte, e);
	pmm_free((void *)pa);                     /* the entry's; the cache keeps the page */
	if (pg->refcount == 1) *wb = pa;
	spin_unlock_irq(&swap_lock, f);
	return 1;
}

/* Write one cache page whose only holder is the cache, then free it. Someone
 * may map it again while it is written: then it stays. 1 when freed. */
static int writeback_one(uint64_t pa)
{
	page_t *pg = paddr_to_page(pa);
	uint64_t f = spin_lock_irq(&swap_lock);
	if (!(pg->flags & PG_SWAPCACHE) || pg->refcount != 1 ||
	    (pg->flags & (PG_SWAPREAD | PG_SWAPWRITE))) {
		spin_unlock_irq(&swap_lock, f);
		return 0;
	}
	swap_area_t *a = area_of(pg->swap);
	uint64_t slot = swp_slot(pg->swap);
	if (!a || cache_trim(a, slot)) { spin_unlock_irq(&swap_lock, f); return a != NULL; }
	pg->flags |= PG_SWAPWRITE;
	spin_unlock_irq(&swap_lock, f);

	int err = swap_io(a, slot, (void *)PHYS_TO_VIRT(pa), 1);

	int freed = 0;
	f = spin_lock_irq(&swap_lock);
	pg->flags &= ~(uint32_t)PG_SWAPWRITE;
	if (!err) swap_nr_out++;
	if (!err && pg->refcount == 1) {
		cache_drop(a, slot, pg);
		freed = pmm_free((void *)pa);
	} else {
		cache_trim(a, slot);
	}
	spin_unlock_irq(&swap_lock, f);
	return freed;
}

long swap_writeback(const uint64_t *pages, int n)
{
	long freed = 0;
	for (int i = 0; i < n; i++)
		if (pages[i]) freed += writeback_one(pages[i]);
	return freed;
}

/* A page for swap-in: reclaim, then the OOM killer, as a fault would. */
static uint64_t swapin_alloc(void)
{
	for (int i = 0; i < 8; i++) {
		void *pg = buddy_alloc(0);
		if (pg) return (uint64_t)pg;
		if (swap_reclaim(32, RECLAIM_ANON) > 0) continue;
		if (oom_kill_alloc_ex(1) != OOM_KILLED_OTHER) break;
	}
	return 0;
}

/* Under swap_lock: put a new page in the cache for slot, being read. */
static void cache_add_reading(swap_area_t *a, uint64_t slot, uint64_t e, uint64_t pa)
{
	page_t *pg = paddr_to_page(pa);
	pmm_hold((void *)pa);                     /* the cache's; the caller keeps the first */
	pg->flags |= PG_ANON | PG_SWAPCACHE | PG_SWAPREAD;
	pg->swap = e;
	a->cache[slot] = pa;
	swap_nr_cached++;
	a->count[slot]++;
}

/* The page of swap entry e, with a reference for the caller: from the cache,
 * or read in. -ENOENT: the entry no longer names a used slot (the caller
 * looks at its page table again). -ENOMEM, -EIO. */
static int swap_in_page(uint64_t e, uint64_t *pap)
{
	uint64_t slot = swp_slot(e), np = 0;
	for (;;) {
		uint64_t f = spin_lock_irq(&swap_lock);
		swap_area_t *a = area_of(e);
		if (!a || a->count[slot] == 0) { spin_unlock_irq(&swap_lock, f); break; }
		uint64_t pa = a->cache[slot];
		if (pa && (paddr_to_page(pa)->flags & PG_SWAPREAD)) {
			sleep_on_locked(&swap_wait, &swap_lock, f);
			continue;
		}
		if (pa) {
			pmm_hold((void *)pa);
			spin_unlock_irq(&swap_lock, f);
			if (np) buddy_free(np, 0);
			*pap = pa;
			return 0;
		}
		if (!np) {                            /* allocate unlocked, then look again */
			spin_unlock_irq(&swap_lock, f);
			if (!(np = swapin_alloc())) return -ENOMEM;
			continue;
		}
		cache_add_reading(a, slot, e, np);
		spin_unlock_irq(&swap_lock, f);

		int err = swap_io(a, slot, (void *)PHYS_TO_VIRT(np), 0);

		f = spin_lock_irq(&swap_lock);
		paddr_to_page(np)->flags &= ~(uint32_t)PG_SWAPREAD;
		if (err) {
			cache_drop(a, slot, paddr_to_page(np));
			pmm_free((void *)np);
		} else {
			swap_nr_in++;
		}
		wake_up(&swap_wait);
		spin_unlock_irq(&swap_lock, f);
		if (err) { pmm_free((void *)np); return err; }
		*pap = np;
		return 0;
	}
	if (np) buddy_free(np, 0);
	return -ENOENT;
}

/* The VMA flags that govern va; no VMA (heap, stack) reads as private RW. */
static uint32_t vma_flags_at(vm_space_t *vs, uint64_t va)
{
	for (vma_t *v = vs->vmas; v; v = v->next)
		if (va >= v->vaddr && va < v->vaddr + v->size) return v->flags;
	return VMA_ANON | VMA_READ | VMA_WRITE;
}

static void sync_icache(uint64_t pa)
{
#if defined(__aarch64__)
	uintptr_t pp = (uintptr_t)PHYS_TO_VIRT(pa);
	for (uintptr_t l = pp; l < pp + PAGE_SIZE; l += 64)
		__asm__ volatile("dc cvac, %0" :: "r"(l) : "memory");
	__asm__ volatile("dsb ish; ic ialluis; dsb ish; isb" ::: "memory");
#else
	(void)pa;
#endif
}

/* Under vs->fault_lock: map pa where swap entry e is. Our reference on pa
 * becomes the mapping's. 1 done, 0 the entry changed meanwhile, -1 a PROT_NONE
 * region: a present page there could not be kept out of reach, so the page
 * stays out on swap, contents kept (swapoff then refuses with EBUSY). */
static int swap_install(vm_space_t *vs, uint64_t va, uint64_t e, uint64_t pa)
{
	uint64_t *pte = arch_pte_find(vs, va);
	if (!pte || *pte != e) return 0;
	uint32_t vf = vma_flags_at(vs, va);
	if (!(vf & VMA_READ)) return -1;
	arch_pte_set(vs, va, pte, 0);
	uint64_t f = spin_lock_irq(&swap_lock);
	swap_area_t *a = area_of(e);
	if (a) {
		slot_put(a, swp_slot(e));             /* the entry's reference moves to the page */
		cache_trim(a, swp_slot(e));
	}
	spin_unlock_irq(&swap_lock, f);
	uint32_t mf = VM_USER | VM_READ;
	if (vf & VMA_EXEC) { mf |= VM_EXEC; sync_icache(pa); }
	/* a page others still hold is read-only: a write takes the COW path */
	if ((vf & VMA_WRITE) && paddr_to_page(pa)->refcount == 1) mf |= VM_WRITE;
	vm_map(vs, va, pa, mf);
	return 1;
}

int swap_fault(vm_space_t *vs, uint64_t va, uint64_t e, int is_write)
{
	(void)is_write;   /* a write to a shared page faults again and copies */
	uint64_t pa = 0;
	unsigned psi = psi_enter(PSI_MEM);         /* a swap-in is a memory stall */
	int r = swap_in_page(e, &pa);
	psi_leave(psi);
	uint64_t lf = spin_lock_irq(&vs->fault_lock);
	uint64_t *pte = arch_pte_find(vs, va);
	int moved = !pte || *pte != e;
	int done = (r == 0) ? swap_install(vs, va, e, pa) : 0;
	spin_unlock_irq(&vs->fault_lock, lf);
	if (r == 0 && done <= 0 && anon_page_put(pa)) cgroup_uncharge_mem(vs, PAGE_SIZE);   /* unmapped meanwhile */
	if (done < 0) return -1;                  /* PROT_NONE: stays out */
	if (r == 0 || moved) return 0;            /* mapped, or someone else resolved it */
	return -1;                                /* no memory, an I/O error, or a bad entry */
}

/* The kernel touched user memory with interrupts off and no lock held (the
 * signal frame on the way back to user mode) and the page was out: a fault
 * there may not sleep, so it failed. Read the range in with interrupts on, as
 * the return to user mode allows, and let the caller try again. */
int swap_user_retry(uint64_t uaddr, size_t len)
{
	process_t *p = task_current();
	if (!p || !p->vm || !len || spin_held_count() > 0) return 0;
	int got = 0;
	uint64_t f = irq_save();
	irq_enable();
	for (uint64_t va = uaddr & PAGE_MASK; va < uaddr + len; va += PAGE_SIZE)
		if (swap_fault_at(p->vm, va) == 1) got = 1;
	irq_restore(f);
	return got;
}

#ifdef ARCH_USER_VA_END
#define USER_END ARCH_USER_VA_END
#else
#define USER_END (1UL << 39)
#endif

void swap_in_all(vm_space_t *vs)
{
	uint64_t va = 0;
	while (va < USER_END) {
		uint64_t e = 0, at = 0, *pte;
		int budget = 4096;
		uint64_t lf = spin_lock_irq(&vs->fault_lock);
		while (budget-- > 0 && (pte = arch_pte_next(vs, &va, USER_END))) {
			if (swp_pte_is(*pte)) { e = *pte; at = va; break; }
			va += PAGE_SIZE;
		}
		spin_unlock_irq(&vs->fault_lock, lf);
		if (e && swap_fault(vs, at, e, 0) != 0) va = at + PAGE_SIZE;   /* cannot: go on */
	}
}

int swap_pte_at(vm_space_t *vs, uint64_t va)
{
	uint64_t lf = spin_lock_irq(&vs->fault_lock);
	uint64_t *pte = arch_pte_find(vs, va & PAGE_MASK);
	int out = pte && swp_pte_is(*pte);
	spin_unlock_irq(&vs->fault_lock, lf);
	return out;
}

int swap_fault_at(vm_space_t *vs, uint64_t va)
{
	va &= PAGE_MASK;
	uint64_t lf = spin_lock_irq(&vs->fault_lock);
	uint64_t *pte = arch_pte_find(vs, va);
	uint64_t e = pte ? *pte : 0;
	spin_unlock_irq(&vs->fault_lock, lf);
	if (!swp_pte_is(e)) return 0;
	return swap_fault(vs, va, e, 0) == 0 ? 1 : -1;
}
