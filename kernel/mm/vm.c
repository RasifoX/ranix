/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
#include <kernel/mm/vm.h>
#include <kernel/mm/buddy.h>
#include <kernel/mm/pmm.h>
#include <kernel/mm/mmu.h>
#include <kernel/mm/hugetlb.h>
#include <kernel/mm/oom.h>
#include <kernel/sync.h>
#include <kernel/hal.h>
#include <kernel/lib/string.h>
#include <kernel/kprintf.h>
#include <kernel/sched.h>
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

static void *vm_alloc_page_oom(void)
{
	void *pg = buddy_alloc(0);
	for (int i = 0; !pg && i < OOM_RETRIES; i++) {
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
static inline int fault_not_present(process_t *p, uint64_t vaddr)
{
	int ret = -1;
	uint32_t mf = 0;
	if (vaddr >= USER_STACK_TOP - MAX_STACK_SIZE && vaddr < USER_STACK_TOP) {
		mf = VM_USER | VM_READ | VM_WRITE;
	} else {
		for (vma_t *v = p->vm->vmas; v; v = v->next) {
			if (vaddr < v->vaddr || vaddr >= v->vaddr + v->size) continue;
			if (!(v->flags & VMA_ANON) || !(v->flags & VMA_READ)) break;
			mf = VM_USER | VM_READ;
			if (v->flags & VMA_WRITE) mf |= VM_WRITE;
			if (v->flags & VMA_EXEC)  mf |= VM_EXEC;
			break;
		}
	}
	if (mf) {
		if (cgroup_charge_mem(p->vm, p->cgroup, PAGE_SIZE) != 0) return ret; /* over memory.max */
		void *pg = vm_alloc_page_oom();
		if (pg) {
			memset((void *)PHYS_TO_VIRT(pg), 0, PAGE_SIZE);
			if (vm_map(p->vm, vaddr, (uint64_t)pg, mf) == 0) {
				ret = 0;
			} else {
				buddy_free((uint64_t)pg, 0);
				cgroup_uncharge_mem(p->vm, PAGE_SIZE);
			}
		} else {
			cgroup_uncharge_mem(p->vm, PAGE_SIZE);
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
                                 uint64_t *cow_old)
{
	int ret = -1;
	if (cgroup_charge_mem(p->vm, p->cgroup, PAGE_SIZE) != 0) return ret; /* over memory.max */
	void *new_p = vm_pmm_page_oom();
	if (!new_p) { cgroup_uncharge_mem(p->vm, PAGE_SIZE); return ret; }
	memcpy((void *)PHYS_TO_VIRT(new_p), (void *)PHYS_TO_VIRT(paddr), PAGE_SIZE);
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

int vm_handle_fault(uint64_t vaddr, int is_write)
{
	vaddr &= PAGE_MASK;
	process_t *p = task_current();
	if (!p || !p->vm) {
		if (!fault_quiet) kprintf(KERN_ERR "mm: fault with no vm pid=%d\n", p ? (int)p->pid : -1);
		return -1;
	}


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
		ret = fault_not_present(p, vaddr);
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
		ret = fault_cow_copy(p, vaddr, paddr, cow_exec, &cow_old);
	} else {
		ret = vm_map(p->vm, vaddr, paddr, VM_USER | VM_READ | VM_WRITE | cow_exec);
	}

out:
	spin_unlock_irq(&p->vm->fault_lock, lf);
	if (cow_old) {
		if (p->vm->refcount > 1) {
			arch_tlb_flush_range(p->vm, vaddr, vaddr + PAGE_SIZE);
		}
		if (pmm_free((void *)cow_old)) cgroup_uncharge_mem(p->vm, PAGE_SIZE);
	}
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
		if (!pg) { spin_unlock_irq(&vs->fault_lock, lf); break; }
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
			if (pmm_free((void *)cow_old)) cgroup_uncharge_mem(vs, PAGE_SIZE);
		}
		done += chunk;
	}
	return done;
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
