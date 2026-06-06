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
	ioremap_vaddr += aligned_size;
	spin_unlock_irq(&vm_lock, f);

	for (uint64_t i = 0; i < aligned_size; i += PAGE_SIZE) {
		vm_map(kernel_vm_space, vaddr + i, aligned_phys + i, flags);
	}

	return (void *)(vaddr + offset);
}

#define USER_STACK_TOP  ARCH_USER_STACK_TOP
#define MAX_STACK_SIZE  (256 * PAGE_SIZE)

int vm_handle_fault(uint64_t vaddr, int is_write)
{
	vaddr &= PAGE_MASK;
	process_t *p = task_current();
	if (!p || !p->vm) { kprintf("fault: no vm pid=%d\n", p ? (int)p->pid : -1); return -1; }

	extern uint64_t arch_vm_get_paddr(uint64_t *root, uint64_t vaddr);

	/* Threads share one address space, so two CPUs can fault the same PTE
	 * at once. Without serialization both would run the COW copy-and-free
	 * and drop the shared page's refcount twice for one logical reference,
	 * freeing it while a forked sibling still maps it. Resolve under the
	 * per-space lock and re-read the PTE so a second arrival sees the
	 * already-private page and only re-maps it. */
	uint64_t lf = spin_lock_irq(&p->vm->fault_lock);
	uint64_t paddr = arch_vm_get_paddr(p->vm->root, vaddr);
	int ret = -1;

	/* No mapping — a stack expansion, or a demand fault into an anonymous VMA
	 * whose pages were dropped (MADV_DONTNEED/FREE) and never re-touched. Both
	 * fault in a fresh zero page; the anon case maps with the VMA's recorded
	 * permissions so a read-only region stays read-only. A VMA without VMA_READ
	 * (PROT_NONE) is not a candidate — leave ret=-1 for SIGSEGV. */
	if (!paddr) {
		uint32_t mf = 0;
		if (vaddr >= USER_STACK_TOP - MAX_STACK_SIZE && vaddr < USER_STACK_TOP) {
			mf = VM_USER | VM_READ | VM_WRITE;
		} else {
			for (vma_t *v = p->vmas; v; v = v->next) {
				if (vaddr < v->vaddr || vaddr >= v->vaddr + v->size) continue;
				if (!(v->flags & VMA_ANON) || !(v->flags & VMA_READ)) break;
				mf = VM_USER | VM_READ;
				if (v->flags & VMA_WRITE) mf |= VM_WRITE;
				if (v->flags & VMA_EXEC)  mf |= VM_EXEC;
				break;
			}
		}
		if (mf) {
			if (cgroup_charge_mem(p->cgroup, PAGE_SIZE) != 0) goto out; /* over memory.max */
			void *pg = vm_alloc_page_oom();
			if (pg) {
				memset((void *)PHYS_TO_VIRT(pg), 0, PAGE_SIZE);
				if (vm_map(p->vm, vaddr, (uint64_t)pg, mf) == 0) {
					p->mem_charged += PAGE_SIZE;
					ret = 0;
				} else {
					buddy_free((uint64_t)pg, 0);
					cgroup_uncharge_mem(p->cgroup, PAGE_SIZE);
				}
			} else {
				cgroup_uncharge_mem(p->cgroup, PAGE_SIZE);
			}
		}
		goto out;
	}

	if (!is_write) goto out;

	/* Find the VMA covering this page. VMA_SHARED is mapped writable directly
	 * (not COW'd). A write to a page whose VMA lacks VMA_WRITE is a real
	 * protection violation — e.g. an mprotect'd read-only region — so leave
	 * ret = -1 and let the caller raise SIGSEGV. Pages with no VMA (heap/brk)
	 * fall through to the COW path below. */
	uint32_t cow_exec = 0;   /* preserve executability across the COW remap */
	for (vma_t *v = p->vmas; v; v = v->next) {
		if (vaddr >= v->vaddr && vaddr < v->vaddr + v->size) {
			if (v->flags & VMA_EXEC) cow_exec = VM_EXEC;
			if (v->flags & VMA_HUGE_1G) {
				/* 1 GB hugepage COW: copy the whole block (hugetlb pages are
				 * never split). Shared or sole-owner blocks just go writable. */
				if (!(v->flags & VMA_WRITE)) goto out;
				uint64_t base = paddr & HUGE_1G_MASK;
				uint64_t va1g = vaddr & HUGE_1G_MASK;
				if (!(v->flags & VMA_SHARED) && huge1g_refcount(base) > 1) {
					uint64_t nb = huge1g_alloc();
					if (!nb) goto out;
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
				goto out;
			}
			if (v->flags & VMA_SHARED) {
				if (!(v->flags & VMA_WRITE)) goto out;   /* read-only shared: real protection fault */
				/* A file-backed shared page (not VMA_ANON) is the inode's own buffer,
				 * kept off teardown's free path — preserve VM_NOFREE when remapping it
				 * writable so vm_destroy doesn't free a page the file still owns. */
				uint32_t wf = VM_USER | VM_READ | VM_WRITE | cow_exec;
				if (!(v->flags & VMA_ANON)) wf |= VM_NOFREE;
				ret = vm_map(p->vm, vaddr, paddr, wf);
				goto out;
			}
			if (!(v->flags & VMA_WRITE)) goto out;
			break;
		}
	}

	/* Permission fault on a mapped page — COW */
	page_t *pg = paddr_to_page(paddr);
	if (!pg) goto out;

	if (pg->refcount > 1) {
		if (cgroup_charge_mem(p->cgroup, PAGE_SIZE) != 0) goto out; /* over memory.max */
		void *new_p = vm_pmm_page_oom();
		if (!new_p) { cgroup_uncharge_mem(p->cgroup, PAGE_SIZE); goto out; }
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
			pmm_free((void *)paddr);
			p->mem_charged += PAGE_SIZE;
			ret = 0;
		} else {
			pmm_free(new_p);
			cgroup_uncharge_mem(p->cgroup, PAGE_SIZE);
		}
	} else {
		ret = vm_map(p->vm, vaddr, paddr, VM_USER | VM_READ | VM_WRITE | cow_exec);
	}

out:
	spin_unlock_irq(&p->vm->fault_lock, lf);
	return ret;
}

void vm_activate(void)
{
	task_t *t = task_current();
	if ((uint64_t)t < 0xFFFF000000000000ULL) {
		kprintf("[DBG] vm_activate: current=%p\n", t);
		kpanic("vm_activate: invalid cpu->current");
	}
	if (t->vm) {
		arch_set_user_kstack((uint64_t)t->kstack + KSTACK_SIZE);
		arch_vm_switch((uint64_t)t->vm->root, t->vm->asid);
	} else if (kernel_pt_phys) {
		arch_vm_switch(kernel_pt_phys, 0);
	}
}
