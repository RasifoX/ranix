/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
#include <kernel/psi.h>
#include <kernel/mm/vm.h>
#include <kernel/mm/buddy.h>
#include <kernel/mm/pmm.h>
#include <kernel/mm/mmu.h>
#include <kernel/mm/hugetlb.h>
#include <kernel/mm/oom.h>
#include <kernel/mm/swap.h>
#include <kernel/irq.h>
#include <kernel/sync.h>
#include <kernel/hal.h>
#include <kernel/lib/string.h>
#include <kernel/kprintf.h>
#include <kernel/sched.h>
#include <kernel/smp.h>
#include <kernel/vfs.h>
#include <kernel/cgroup.h>
#include <asm/mmu.h>

/* Buddy retries under memory pressure: 0 → kill the biggest user process so its
 * exit frees memory, then retry. Bounded so a kill that frees nothing (e.g. the
 * faulting hog is the victim and only dies on return) can't spin forever. The
 * caller holds the per-space fault_lock; oom_kill_alloc takes only sched_lock
 * (fault_lock → sched_lock is the sole order — no ABBA), never fault_lock, so it
 * cannot self-deadlock. */
#define OOM_RETRIES 8

/* With swap on, a fault that finds no free page first gives up the lock and
 * swaps pages out (FAULT_NOMEM back to vm_handle_fault); only a fault that may
 * kill runs the OOM killer. */
#define FAULT_NOMEM   (-2)
#define RECLAIM_PASSES 4

static void *vm_alloc_page_oom(int may_kill)
{
	void *pg = buddy_alloc(0);
	for (int i = 0; !pg && may_kill && i < OOM_RETRIES; i++) {
		if (oom_kill_alloc() != OOM_KILLED_OTHER) break;
		pg = buddy_alloc(0);
	}
	return pg;
}

static void *vm_pmm_page_oom(void)
{
	void *pg = pmm_alloc();
	for (int i = 0; !pg && i < OOM_RETRIES; i++) {
		if (oom_kill_alloc() != OOM_KILLED_OTHER) break;
		pg = pmm_alloc();
	}
	return pg;
}

static uint64_t kernel_pt_phys;
static vm_space_t _kernel_vm_space;
vm_space_t *kernel_vm_space = &_kernel_vm_space;

#ifdef __aarch64__
static uint64_t ioremap_vaddr = 0xFFFFFFC000000000UL;
#else
static uint64_t ioremap_vaddr = 0xFFFFFFFFC0000000UL;
#endif

static spinlock_t vm_lock = SPINLOCK_INIT;

void vm_set_kernel_pt(uint64_t phys)
{
	kernel_pt_phys = phys;
}

void vm_init_kernel(uint64_t root_phys)
{
	kernel_pt_phys = root_phys;
	_kernel_vm_space.root = (uint64_t *)root_phys;
	_kernel_vm_space.magic = VM_SPACE_MAGIC;
}

void *ioremap(uint64_t phys_addr, size_t size, uint32_t flags)
{
	uint64_t aligned_phys = phys_addr & PAGE_MASK;
	uint64_t offset       = phys_addr & ~PAGE_MASK;
	uint64_t aligned_size = (size + offset + PAGE_SIZE - 1) & PAGE_MASK;

	uint64_t f = spin_lock_irq(&vm_lock);
	uint64_t vaddr = ioremap_vaddr;
	/* The window runs to the top of the address space and is never given back.
	 * Past it the cursor wraps to low addresses, whose page tables the kernel
	 * image shares on x86 (boot pd), and the mapping lands on kernel text. */
	if (aligned_size >= 0 - vaddr) {
		spin_unlock_irq(&vm_lock, f);
		kprintf(KERN_ERR "mm: ioremap window exhausted, %lu bytes refused\n", (unsigned long)aligned_size);
		return NULL;
	}
	ioremap_vaddr += aligned_size;
	spin_unlock_irq(&vm_lock, f);

	for (uint64_t i = 0; i < aligned_size; i += PAGE_SIZE) {
		vm_map(kernel_vm_space, vaddr + i, aligned_phys + i, flags);
	}

	return (void *)(vaddr + offset);
}

#define USER_STACK_TOP  ARCH_USER_STACK_TOP
#define MAX_STACK_SIZE  (256 * PAGE_SIZE)

/* Slot-0 invariant: vm_copy/vm_destroy walk only the top-level user slot (x86
 * PML4 index 0, aarch64 L0[0]), each covering the first 512 GB of user VA. The
 * whole user layout — stack top, the 1 GB-hugepage window, and the mmap ceiling
 * below it — must stay inside that slot, or fork would skip and exit would leak
 * the mappings above it (a COW-bypass / page leak). */
#ifndef ARCH_USER_PT_SLOTS
_Static_assert(ARCH_USER_STACK_TOP <= 0x8000000000UL,
               "user stack top must stay within page-table slot 0 (512 GB)");
_Static_assert(HUGE1G_VA_BASE < 0x8000000000UL,
               "1 GB-hugepage window must stay within page-table slot 0 (512 GB)");
#else
_Static_assert(ARCH_USER_STACK_TOP < ARCH_USER_VA_END && HUGE1G_VA_BASE < ARCH_USER_STACK_TOP,
               "user layout must stay inside the user page-table slots");
#endif

	/* No mapping — a stack expansion, or a demand fault into an anonymous VMA
	 * whose pages were dropped (MADV_DONTNEED/FREE) and never re-touched. Both
	 * fault in a fresh zero page; the anon case maps with the VMA's recorded
	 * permissions so a read-only region stays read-only. A VMA without VMA_READ
	 * (PROT_NONE) is not a candidate — leave ret=-1 for SIGSEGV. */
static inline int fault_not_present(process_t *p, uint64_t vaddr, int may_kill)
{
	int ret = -1;
	uint32_t mf = 0;
	int anon = 1;   /* a private page: it may go to swap */
	if (vaddr >= USER_STACK_TOP - MAX_STACK_SIZE && vaddr < USER_STACK_TOP) {
		mf = VM_USER | VM_READ | VM_WRITE;
	} else {
		for (vma_t *v = p->vm->vmas; v; v = v->next) {
			if (vaddr < v->vaddr || vaddr >= v->vaddr + v->size) continue;
			if (!(v->flags & VMA_ANON) || !(v->flags & VMA_READ)) break;
			mf = VM_USER | VM_READ;
			if (v->flags & VMA_WRITE) mf |= VM_WRITE;
			if (v->flags & VMA_EXEC)  mf |= VM_EXEC;
			anon = !(v->flags & VMA_SHARED);
			break;
		}
	}
	if (mf) {
		if (cgroup_charge_mem(p->vm, p->cgroup, PAGE_SIZE) != 0) return ret; /* over memory.max */
		void *pg = vm_alloc_page_oom(may_kill);
		if (pg) {
			memset((void *)PHYS_TO_VIRT(pg), 0, PAGE_SIZE);
			if (anon) anon_page_mark((uint64_t)pg);
			if (vm_map(p->vm, vaddr, (uint64_t)pg, mf) == 0) {
				ret = 0;
			} else {
				buddy_free((uint64_t)pg, 0);
				cgroup_uncharge_mem(p->vm, PAGE_SIZE);
			}
		} else {
			cgroup_uncharge_mem(p->vm, PAGE_SIZE);
			if (!may_kill) ret = FAULT_NOMEM;
		}
	}
	return ret;
}

	/* 1 GB hugepage COW: copy the whole block (hugetlb pages are
	 * never split). Shared or sole-owner blocks just go writable. */
static inline int fault_huge_1g(process_t *p, vma_t *v, uint64_t vaddr, uint64_t paddr)
{
	int ret = -1;
	if (!(v->flags & VMA_WRITE)) return ret;
	uint64_t base = paddr & HUGE_1G_MASK;
	uint64_t va1g = vaddr & HUGE_1G_MASK;
	if (!(v->flags & VMA_SHARED) && huge1g_refcount(base) > 1) {
		uint64_t nb = huge1g_alloc();
		if (!nb) return ret;
		memcpy((void *)PHYS_TO_VIRT(nb), (void *)PHYS_TO_VIRT(base), HUGE_1G_SIZE);
		if (vm_map_huge_1g(p->vm, va1g, nb, VM_USER | VM_READ | VM_WRITE) == 0) {
			huge1g_put(base);
			ret = 0;
		} else {
			huge1g_put(nb);
		}
	} else {
		ret = vm_map_huge_1g(p->vm, va1g, base, VM_USER | VM_READ | VM_WRITE);
	}
	return ret;
}

/* Write to a page shared after fork: copy it, map the copy writable, and hand
 * back the old page (*cow_old) to free once fault_lock is dropped. */
static inline int fault_cow_copy(process_t *p, uint64_t vaddr, uint64_t paddr, uint32_t cow_exec,
                                 uint64_t *cow_old, int may_kill)
{
	int ret = -1;
	if (cgroup_charge_mem(p->vm, p->cgroup, PAGE_SIZE) != 0) return ret; /* over memory.max */
	void *new_p = vm_alloc_page_oom(may_kill);
	if (!new_p) { cgroup_uncharge_mem(p->vm, PAGE_SIZE); return may_kill ? ret : FAULT_NOMEM; }
	memcpy((void *)PHYS_TO_VIRT(new_p), (void *)PHYS_TO_VIRT(paddr), PAGE_SIZE);
	anon_page_mark((uint64_t)new_p);   /* the copy is this space's own */
#if defined(__aarch64__)
	if (cow_exec) {
		uintptr_t pp = (uintptr_t)PHYS_TO_VIRT(new_p);
		for (uintptr_t l = pp; l < pp + PAGE_SIZE; l += 64)
			__asm__ volatile("dc cvac, %0" :: "r"(l) : "memory");
		__asm__ volatile("dsb ish; ic ialluis; dsb ish; isb" ::: "memory");
	}
#endif
	if (vm_map(p->vm, vaddr, (uint64_t)new_p, VM_USER | VM_READ | VM_WRITE | cow_exec) == 0) {
		/* A multithreaded space shares this page table across CPUs: a sibling
		 * thread may hold a stale writable TLB entry for the old page. Defer the
		 * free and shoot the other CPUs down past fault_lock — a sibling spinning
		 * for the lock IRQ-off must be able to ack the IPI. */
		*cow_old = paddr;
		ret = 0;
	} else {
		pmm_free(new_p);
		cgroup_uncharge_mem(p->vm, PAGE_SIZE);
	}
	return ret;
}

/* Set around a fault the kernel causes on purpose (the null page selftest):
 * the error log is for faults nobody expected. */
static int fault_quiet;
void vm_fault_quiet(int on) { fault_quiet = on; }

/* The page at vaddr is out on swap (e, read under fault_lock, which the
 * caller holds): drop the lock and read it in. A PROT_NONE region stays out,
 * and so does a page the kernel touched under a spinlock: reading may sleep. */
static int fault_swapped(process_t *p, uint64_t vaddr, uint64_t e, int is_write, uint64_t lf, int may_sleep)
{
	int readable = may_sleep;
	for (vma_t *v = p->vm->vmas; v; v = v->next)
		if (vaddr >= v->vaddr && vaddr < v->vaddr + v->size) { readable &= (v->flags & VMA_READ) != 0; break; }
	spin_unlock_irq(&p->vm->fault_lock, lf);
	return readable ? swap_fault(p->vm, vaddr, e, is_write) : -1;
}

/* One try at the fault under the space's fault_lock. may_kill: an allocation
 * that fails runs the OOM killer; else it returns FAULT_NOMEM. may_sleep: no
 * spinlock was held at the fault, so a page may be read in from swap. */
static int fault_resolve(process_t *p, uint64_t vaddr, int is_write, int may_kill, int may_sleep)
{
	/* Threads share one address space, so two CPUs can fault the same PTE
	 * at once. Without serialization both would run the COW copy-and-free
	 * and drop the shared page's refcount twice for one logical reference,
	 * freeing it while a forked sibling still maps it. Resolve under the
	 * per-space lock and re-read the PTE so a second arrival sees the
	 * already-private page and only re-maps it. */
	uint64_t lf = spin_lock_irq(&p->vm->fault_lock);
	uint64_t paddr = arch_vm_get_paddr(p->vm->root, vaddr);
	int ret = -1;
	uint64_t cow_old = 0;   /* old COW page to free + shoot down after fault_lock */

	if (!paddr) {
		uint64_t *pte = arch_pte_find(p->vm, vaddr);
		if (pte && swp_pte_is(*pte)) return fault_swapped(p, vaddr, *pte, is_write, lf, may_sleep);
		ret = fault_not_present(p, vaddr, may_kill);
		goto out;
	}

	/* A read that found the page present lost the race to a sibling's fault on
	 * the same page: the handlers only come here for a not-present read. */
	if (!is_write) { ret = 0; goto out; }

	/* Find the VMA covering this page. VMA_SHARED is mapped writable directly
	 * (not COW'd). A write to a page whose VMA lacks VMA_WRITE is a real
	 * protection violation — e.g. an mprotect'd read-only region — so leave
	 * ret = -1 and let the caller raise SIGSEGV. Pages with no VMA (heap/brk)
	 * fall through to the COW path below. */
	uint32_t cow_exec = 0;   /* preserve executability across the COW remap */
	int in_vma = 0;
	for (vma_t *v = p->vm->vmas; v; v = v->next) {
		if (vaddr >= v->vaddr && vaddr < v->vaddr + v->size) {
			if (v->flags & VMA_EXEC) cow_exec = VM_EXEC;
			if (v->flags & VMA_HUGE_1G) {
				ret = fault_huge_1g(p, v, vaddr, paddr);
				goto out;
			}
			if (v->flags & VMA_SHARED) {
				if (!(v->flags & VMA_WRITE)) goto out;   /* read-only shared: real protection fault */
				ret = vm_map(p->vm, vaddr, paddr, VM_USER | VM_READ | VM_WRITE | cow_exec);
				goto out;
			}
			if (!(v->flags & VMA_WRITE)) goto out;
			in_vma = 1;
			break;
		}
	}

	/* Permission fault on a mapped page — COW */
	page_t *pg = paddr_to_page(paddr);
	if (!pg) {
		/* No page behind it: device memory has nothing to copy, so a writable
		 * mapping of it just goes writable. vm_map keeps its cache type. */
		if (in_vma) ret = vm_map(p->vm, vaddr, paddr, VM_USER | VM_READ | VM_WRITE | cow_exec);
		goto out;
	}

	if (pg->refcount > 1) {
		ret = fault_cow_copy(p, vaddr, paddr, cow_exec, &cow_old, may_kill);
	} else {
		ret = vm_map(p->vm, vaddr, paddr, VM_USER | VM_READ | VM_WRITE | cow_exec);
	}

out:
	spin_unlock_irq(&p->vm->fault_lock, lf);
	if (cow_old) {
		if (vm_tlb_elsewhere(p->vm))
			arch_tlb_flush_range(p->vm, vaddr, vaddr + PAGE_SIZE);
		if (anon_page_put(cow_old)) cgroup_uncharge_mem(p->vm, PAGE_SIZE);
	}
	return ret;
}

/* Another CPU may still translate through vs: a thread of it runs there, or the
 * CPU ran it last and kept its TLB (x86 arch_vm_switch skips the CR3 reload when
 * the same space comes back, and an idle CPU keeps the last one). A single-threaded
 * process that migrated away from a CPU and back, past an unmap done meanwhile
 * without a shootdown, wrote through the stale entry into the freed page: kernel
 * heap pages zeroed under the syscall fuzzer. */
int vm_tlb_elsewhere(vm_space_t *vs)
{
	if (vs->refcount > 1) return 1;   /* a sibling thread may run there now: shoot down */
	/* One thread, running here: the others only kept the space's entries from
	 * before. Forgetting their tag makes their next switch to it reload CR3,
	 * which flushes; no IPI. */
	cpu_context_t *self = arch_get_cpu_ctx();
	for (int i = 0; i < smp_ncpus && i < MAX_CPUS; i++) {
		cpu_context_t *c = all_cpus[i];
		if (c && c != self && c->vm_tag == vs->asid)
			__atomic_store_n(&c->vm_tag, 0, __ATOMIC_RELEASE);
	}
	return 0;
}

/* Unmap [lo, hi) a batch at a time: one shootdown for up to VM_UNMAP_BATCH pages
 * instead of one per page. A threaded process giving back a 2 GB heap paid half
 * a million IPI round trips that way, 15 s in the kernel without a yield (found
 * by test_fuzz). arch_pte_next shows only 4 KB entries; huge mappings go through
 * vm_unmap in the second pass, which finds the cleared entries empty and so
 * sends no further IPIs. */
#define VM_UNMAP_BATCH 128
void vm_unmap_range(vm_space_t *vs, uint64_t lo, uint64_t hi)
{
	lo &= PAGE_MASK;
	for (uint64_t va = lo; va < hi; ) {
		uint64_t old[VM_UNMAP_BATCH];
		int n = 0;
		uint64_t *pte = NULL;
		uint64_t lf = spin_lock_irq(&vs->fault_lock);
		while (n < VM_UNMAP_BATCH && (pte = arch_pte_next(vs, &va, hi))) {
			if (*pte) { old[n++] = *pte; arch_pte_set(vs, va, pte, 0); }
			va += PAGE_SIZE;
		}
		spin_unlock_irq(&vs->fault_lock, lf);
		if (n && vm_tlb_elsewhere(vs)) arch_tlb_flush_range(vs, lo, va);
		for (int i = 0; i < n; i++) {
			uint64_t pa = arch_pte_page(old[i]);
			if (pa) { if (anon_page_put(pa)) cgroup_uncharge_mem(vs, PAGE_SIZE); }
			else if (swp_pte_is(old[i]) && swap_pte_free(old[i])) cgroup_uncharge_mem(vs, PAGE_SIZE);
		}
		cond_resched();
	}
	int steps = 0;
	for (uint64_t v = lo; v < hi; ) {
		uint64_t nx = vm_skip_empty(vs, v);
		if (nx > v) { v = nx; continue; }
		vm_unmap(vs, v);
		v += PAGE_SIZE;
		if ((++steps & 4095) == 0) cond_resched();
	}
}

int vm_handle_fault(uint64_t vaddr, int is_write)
{
	return vm_handle_fault_irq(vaddr, is_write, 0);
}

/* irqs_on: the faulting context had interrupts on (a user page, or a syscall
 * copying user memory). Only then may the fault sleep - reclaim, a swap read:
 * the exception came in with interrupts off, and a CPU that sleeps with them
 * off never wakes. They are turned back on for that work, as Linux does. */
int vm_handle_fault_irq(uint64_t vaddr, int is_write, int irqs_on)
{
	vaddr &= PAGE_MASK;
	process_t *p = task_current();
	if (!p || !p->vm) {
		if (!fault_quiet) kprintf(KERN_ERR "mm: fault with no vm pid=%d\n", p ? (int)p->pid : -1);
		return -1;
	}

	/* Reclaim may sleep: not for a fault the kernel took under a spinlock. */
	int may_sleep = irqs_on && spin_held_count() == 0;
	uint64_t irqf = irq_save();
	if (may_sleep) { irq_enable(); swap_memory_check(); }
	int ret;
	for (int pass = 0; ; pass++) {
		int last = !may_sleep || !swap_active() || pass >= RECLAIM_PASSES;
		ret = fault_resolve(p, vaddr, is_write, last, may_sleep);
		if (ret != FAULT_NOMEM) break;
		unsigned psi = psi_enter(PSI_MEM);   /* a fault that waits for reclaim */
		swap_reclaim(32, RECLAIM_ANON);
		psi_leave(psi);
	}
	irq_restore(irqf);
	return ret;
}

/* Copy between kbuf and another address space, page by page through the direct
 * map under that space's fault_lock. Nothing is dereferenced by user address, so
 * an unmapped or read-only target page cannot fault the kernel, and a write
 * breaks COW in the target rather than reaching its fork relatives. Returns the
 * bytes copied. Device memory has no direct-map alias and stops the copy. */
size_t vm_access_remote(vm_space_t *vs, uint64_t va, void *kbuf, size_t n, int write)
{
	process_t *cur = task_current();
	size_t done = 0;
	while (done < n) {
		uint64_t a = va + done, pgva = a & PAGE_MASK;
		size_t off = a - pgva, chunk = PAGE_SIZE - off;
		if (chunk > n - done) chunk = n - done;
		if (!access_ok((void *)a, chunk)) break;

		uint64_t cow_old = 0;
		uint64_t lf = spin_lock_irq(&vs->fault_lock);
		uint64_t pa = arch_vm_get_paddr(vs->root, pgva);
		page_t *pg = pa ? paddr_to_page(pa) : NULL;
		if (!pg) {
			spin_unlock_irq(&vs->fault_lock, lf);
			if (!pa && swap_fault_at(vs, pgva) == 1) continue;   /* was out on swap: read in, again */
			break;
		}
		if (write) {
			uint32_t vf = 0;
			for (vma_t *v = vs->vmas; v; v = v->next)
				if (pgva >= v->vaddr && pgva < v->vaddr + v->size) { vf = v->flags; break; }
			if (vf & VMA_HUGE_1G) { spin_unlock_irq(&vs->fault_lock, lf); break; }
			if (!(vf & VMA_SHARED) && pg->refcount > 1) {
				void *np = NULL;
				if (cgroup_charge_mem(vs, cur->cgroup, PAGE_SIZE) == 0) {
					np = vm_pmm_page_oom();
					if (!np) cgroup_uncharge_mem(vs, PAGE_SIZE);
				}
				if (!np) { spin_unlock_irq(&vs->fault_lock, lf); break; }
				memcpy((void *)PHYS_TO_VIRT(np), (void *)PHYS_TO_VIRT(pa), PAGE_SIZE);
				anon_page_mark((uint64_t)np);
				/* stays read-only: the target's own next write takes the normal path */
				uint32_t mf = VM_USER | VM_READ | ((vf & VMA_EXEC) ? VM_EXEC : 0);
				if (vm_map(vs, pgva, (uint64_t)np, mf) != 0) {
					pmm_free(np);
					cgroup_uncharge_mem(vs, PAGE_SIZE);
					spin_unlock_irq(&vs->fault_lock, lf);
					break;
				}
				cow_old = pa;
				pa = (uint64_t)np;
			}
			memcpy((uint8_t *)PHYS_TO_VIRT(pa) + off, (uint8_t *)kbuf + done, chunk);
		} else {
			memcpy((uint8_t *)kbuf + done, (uint8_t *)PHYS_TO_VIRT(pa) + off, chunk);
		}
		spin_unlock_irq(&vs->fault_lock, lf);
		if (cow_old) {
			arch_tlb_flush_range(vs, pgva, pgva + PAGE_SIZE);
			if (anon_page_put(cow_old)) cgroup_uncharge_mem(vs, PAGE_SIZE);
		}
		done += chunk;
	}
	return done;
}

/* The caller's page at va, made present (and private and writable for write)
 * through the normal fault path, then held with one extra reference. A device
 * that reaches the page by its physical address (a KVM guest's memory through
 * EPT) cannot take a page fault, so the page must stay where it is: swap and the
 * 2 MB collapse only move a page nobody else holds. The write fault comes first,
 * while this reference is not there yet, so it breaks COW rather than copying
 * the page away from the hold. Hugepage regions are refused: their 4 KB parts
 * are not separate pages to hold. Returns the physical address, 0 on failure. */
uint64_t vm_pin_user_page(uint64_t va, int write)
{
	process_t *p = task_current();
	if (!p || !p->vm || !access_ok((void *)va, PAGE_SIZE)) return 0;
	vm_space_t *vs = p->vm;
	va &= PAGE_MASK;
	for (int tries = 0; tries < 3; tries++) {
		if (vm_handle_fault_irq(va, write, 1) != 0) return 0;
		uint64_t lf = spin_lock_irq(&vs->fault_lock);
		uint32_t vf = 0;
		for (vma_t *v = vs->vmas; v; v = v->next)
			if (va >= v->vaddr && va < v->vaddr + v->size) { vf = v->flags; break; }
		uint64_t pa = arch_vm_get_paddr(vs->root, va);
		page_t *pg = pa ? paddr_to_page(pa) : NULL;
		if ((vf & (VMA_HUGE | VMA_HUGE_1G)) || (pa && (!pg || !(pg->flags & PG_MANAGED)))) {
			spin_unlock_irq(&vs->fault_lock, lf);
			return 0;
		}
		if (pg && (!write || (vf & VMA_WRITE))) {
			pmm_hold((void *)pa);
			spin_unlock_irq(&vs->fault_lock, lf);
			return pa;
		}
		spin_unlock_irq(&vs->fault_lock, lf);   /* swapped out again meanwhile: once more */
	}
	return 0;
}

/* Drop a vm_pin_user_page hold. vs is the space it was taken in: when this was
 * the last reference (the owner unmapped the page meanwhile), the memory charge
 * the owner's unmap left in place goes with it. */
void vm_unpin_user_page(vm_space_t *vs, uint64_t pa)
{
	page_t *pg = paddr_to_page(pa);
	int anon = pg && (pg->flags & PG_ANON);
	if (anon_page_put(pa) && anon && vs) cgroup_uncharge_mem(vs, PAGE_SIZE);
}

void vm_activate(void)
{
	task_t *t = task_current();
	if ((uint64_t)t < 0xFFFF000000000000ULL) {
		kprintf(KERN_ERR "mm: vm_activate bad current=%p\n", t);
		kpanic("vm_activate: invalid cpu->current");
	}
	if (t->vm) {
		arch_set_user_kstack((uint64_t)t->kstack + KSTACK_SIZE);
		arch_vm_switch((uint64_t)t->vm->root, t->vm->asid);
	} else if (kernel_pt_phys) {
		arch_vm_switch(kernel_pt_phys, 0);
	}
}
