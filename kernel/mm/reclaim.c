/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* Reclaim: which anonymous pages go to swap, and when.
 *
 * There is no reverse map, so reclaim walks page tables, one address space at a
 * time, like the hand of a clock: each space keeps where the hand stopped
 * (vs->swap_scan). A page whose accessed bit is set gets a second chance (the
 * bit is cleared and the hand moves on); one found still unused on the next
 * turn goes to swap. Locked, shared and huge-page regions are never touched.
 *
 * When: kswapd wakes when free memory falls under the low mark and works until
 * the high mark; a fault that finds memory under the min mark reclaims itself
 * first, and a fault whose allocation fails swaps before the OOM killer runs.
 * vm.swappiness 0 keeps kswapd and the min-mark check off anonymous pages:
 * they then go only to save a failing allocation. MADV_PAGEOUT sends a range
 * out at once. Design: arastirma/swap.md. */
#include "swap_area.h"
#include <kernel/psi.h>
#include <kernel/mm/pmm.h>
#include <kernel/mm/buddy.h>
#include <kernel/proc.h>
#include <kernel/board.h>
#include <kernel/config.h>
#include <kernel/kprintf.h>
#include <kernel/cmdline.h>
#include <kernel/errno.h>
#include <asm/mmu.h>

int vm_swappiness = 60;

#ifdef ARCH_USER_VA_END
#define USER_END ARCH_USER_VA_END
#else
#define USER_END (1UL << 39)
#endif

#define BATCH       32    /* pages written per fault_lock hold */
#define SCAN_BUDGET 2048  /* page-table entries looked at per fault_lock hold */

static mutex_t      reclaim_lock = MUTEX_INIT;   /* one reclaimer walks at a time */
static unsigned     clock_proc;                  /* the process slot the hand is at */
static spinlock_t   kswapd_lock = SPINLOCK_INIT;
static wait_queue_t kswapd_wq = WAIT_QUEUE_INIT;
static int          kswapd_running;

int swap_active(void)
{
	for (int i = 0; i < SWAP_AREAS; i++)
		if (swap_areas[i].on && !swap_areas[i].draining) return 1;
	return 0;
}

/* Watermarks in pages: min = RAM/256, at least 1 MB; low = 2*min; high = 3*min. */
static uint64_t wmark_min(void)
{
	uint64_t m = buddy_total_pages() / 256;
	return m < 256 ? 256 : m;
}

typedef struct {
	uint64_t wb[BATCH];   /* pages only the swap cache holds now: to write */
	int      nwb;
	int      out;         /* entries turned into swap entries */
	int      full;        /* no free slot left */
	int      all;         /* MADV_PAGEOUT: ignore the accessed bit */
} scan_t;

static int vma_swappable(const vma_t *v)
{
	return !v || !(v->flags & (VMA_SHARED | VMA_LOCKED | VMA_HUGE | VMA_HUGE_1G));
}

static vma_t *vma_at(vm_space_t *vs, uint64_t va)
{
	for (vma_t *v = vs->vmas; v; v = v->next)
		if (va >= v->vaddr && va < v->vaddr + v->size) return v;
	return NULL;
}

/* Under vs->fault_lock: walk [*va, end) turning unused anonymous pages into
 * swap entries, until the batch fills or the budget is spent. Moves *va. */
static void scan_range(vm_space_t *vs, uint64_t *va, uint64_t end, scan_t *s)
{
	vma_t *v = NULL;
	uint64_t *pte;
	int budget = SCAN_BUDGET;
	while (s->nwb < BATCH && budget > 0 && (pte = arch_pte_next(vs, va, end))) {
		uint64_t a = *va;
		*va += PAGE_SIZE;
		budget--;                       /* every entry: a space of swap entries is walked too */
		if (!arch_pte_page(*pte)) continue;
		if (!v || a < v->vaddr || a >= v->vaddr + v->size) v = vma_at(vs, a);
		if (!vma_swappable(v)) { *va = v->vaddr + v->size; continue; }
		if (arch_pte_young(pte) && !s->all) continue;   /* used since the last turn */
		uint64_t wb;
		int r = swap_out_pte(vs, a, pte, &wb);
		if (r < 0) { s->full = 1; return; }
		if (r > 0) { s->out++; if (wb) s->wb[s->nwb++] = wb; }
	}
}

/* After the lock: no CPU may keep using a page that is about to be written. */
static long scan_finish(vm_space_t *vs, scan_t *s)
{
	if (s->out) arch_tlb_flush_range(vs, 0, 0);
	return swap_writeback(s->wb, s->nwb);
}

/* The hand over one space: up to want pages freed, at most two turns. Pages
 * others share only free when every sharer let go, so the entries turned per
 * call are bounded too: one space is not emptied for another's pages. */
static long reclaim_space(vm_space_t *vs, long want, int *full)
{
	long freed = 0, out = 0;
	if (vs->locked_all) return 0;     /* mlockall: heap and stack have no VMA to carry the lock */
	for (int turns = 0; freed < want && out < 4 * want + BATCH && turns < 2; ) {
		scan_t s = { .nwb = 0 };
		uint64_t lf = spin_lock_irq(&vs->fault_lock);
		uint64_t va = vs->swap_scan;
		scan_range(vs, &va, USER_END, &s);
		if (va >= USER_END) { va = 0; turns++; }
		vs->swap_scan = va;
		spin_unlock_irq(&vs->fault_lock, lf);
		freed += scan_finish(vs, &s);
		out += s.out;
		if (s.full) { *full = 1; break; }
	}
	return freed;
}

static long reclaim_anon(long want)
{
	long got = 0;
	mutex_lock(&reclaim_lock);
	got += swap_writeback_pending();   /* cache pages whose last mapper let go */
	int n = task_count();
	for (int i = 0; i < n && got < want; i++) {
		int slot = (int)(clock_proc++ % (unsigned)(n ? n : 1));
		vm_space_t *vs = proc_get_vm(slot);
		if (!vs) continue;
		int full = 0;
		got += reclaim_space(vs, want - got, &full);
		vm_destroy(vs);                  /* the pin */
		if (full) break;
	}
	mutex_unlock(&reclaim_lock);
	return got;
}

long swap_reclaim(long want, int flags)
{
	long got = 0;
	int anon = (flags & RECLAIM_ANON) && swap_active();
#if IS_ENABLED(CONFIG_FS_EXT2)
	/* clean disk cache first, in the share swappiness leaves it; counted by
	 * the pages that came free, as cache blocks are smaller than pages */
	long cache_want = anon ? want * (200 - vm_swappiness) / 200 : want;
	uint64_t before = buddy_free_pages();
	if (cache_want > 0) ext2_shrink((uint64_t)cache_want * (PAGE_SIZE / 1024));
	uint64_t after = buddy_free_pages();
	if (after > before) got += (long)(after - before);
#endif
	if (anon && got < want) got += reclaim_anon(want - got);
	return got;
}

long swap_pageout(vm_space_t *vs, uint64_t start, uint64_t end)
{
	long out = 0;
	uint64_t va = start;
	while (swap_active() && va < end) {
		scan_t s = { .all = 1 };
		uint64_t lf = spin_lock_irq(&vs->fault_lock);
		scan_range(vs, &va, end, &s);
		spin_unlock_irq(&vs->fault_lock, lf);
		scan_finish(vs, &s);
		out += s.out;
		if (s.full) break;
	}
	return out;
}

static void kswapd_wake(void)
{
	uint64_t f = spin_lock_irq(&kswapd_lock);
	wake_up(&kswapd_wq);
	spin_unlock_irq(&kswapd_lock, f);
}

void swap_memory_check(void)
{
	if (!swap_active() || vm_swappiness == 0) return;
	uint64_t free = buddy_free_pages(), min = wmark_min();
	if (free < 2 * min) kswapd_wake();
	if (free < min) { unsigned psi = psi_enter(PSI_MEM); swap_reclaim(BATCH, RECLAIM_ANON); psi_leave(psi); }
}

/* Keep free memory between the low and high marks while swap is on. Sleeps up
 * to a second at a time, so a missed wake costs at most that. */
static void kswapd(void)
{
	for (;;) {
		uint64_t f = spin_lock_irq(&kswapd_lock);
		sleep_on_locked_until(&kswapd_wq, &kswapd_lock, f, kernel_ticks + TIMER_HZ);
		if (!swap_active() || vm_swappiness == 0) continue;
		uint64_t min = wmark_min();
		if (buddy_free_pages() >= 2 * min) continue;
		while (buddy_free_pages() < 3 * min && swap_active())
			if (swap_reclaim(BATCH, RECLAIM_ANON) <= 0) break;
	}
}

/* Test kernels only. Booted with ranix.test_hooks, a process may ask (prctl
 * PR_RANIX_TEST_HOOK) that the page mprotect or a THP collapse is about to
 * rewrite be swapped out first, right where reclaim on another CPU could do
 * it: a narrow race made certain, so its test fails on code that mishandles
 * it. Without the boot flag the prctl is EINVAL and the hook never fires.
 * arg2 is a bit set: 1 this swap race, 2 a pause inside a pty master's close
 * (kernel/fs/pty.c), 0 none. */
long swap_test_hook_set(uint64_t on)
{
	process_t *p = task_current();
	if (!cmdline_has("ranix.test_hooks") || on > 3 || !p || !p->vm) return -EINVAL;
	p->vm->test_hook = (uint32_t)on;
	return 0;
}

void swap_test_race(vm_space_t *vs, uint64_t va)
{
	if (vs->test_hook & 1) swap_pageout(vs, va & PAGE_MASK, (va & PAGE_MASK) + PAGE_SIZE);
}

void swap_kswapd_start(void)
{
	if (__atomic_exchange_n(&kswapd_running, 1, __ATOMIC_ACQ_REL)) return;
	proc_create("kswapd", kswapd);
}
