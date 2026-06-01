#include <kernel/mm/vm.h>
#include <kernel/mm/buddy.h>
#include <kernel/mm/pmm.h>
#include <kernel/mm/mmu.h>
#include <kernel/mm/hugetlb.h>
#include <kernel/sync.h>
#include <kernel/hal.h>
#include <kernel/lib/string.h>
#include <kernel/kprintf.h>
#include <kernel/sched.h>
#include <kernel/vfs.h>
#include <kernel/cgroup.h>
#include <asm/mmu.h>

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

	/* No mapping — check if this is a stack expansion */
	if (!paddr) {
		if (vaddr >= USER_STACK_TOP - MAX_STACK_SIZE && vaddr < USER_STACK_TOP) {
			if (cgroup_charge_mem(p->cgroup, PAGE_SIZE) != 0) goto out; /* over memory.max */
			void *pg = buddy_alloc(0);
			if (pg) {
				memset((void *)PHYS_TO_VIRT(pg), 0, PAGE_SIZE);
				if (vm_map(p->vm, vaddr, (uint64_t)pg, VM_USER | VM_READ | VM_WRITE) == 0) {
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
	for (vma_t *v = p->vmas; v; v = v->next) {
		if (vaddr >= v->vaddr && vaddr < v->vaddr + v->size) {
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
				ret = vm_map(p->vm, vaddr, paddr, VM_USER | VM_READ | VM_WRITE);
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
		void *new_p = pmm_alloc();
		if (!new_p) { cgroup_uncharge_mem(p->cgroup, PAGE_SIZE); goto out; }
		memcpy((void *)PHYS_TO_VIRT(new_p), (void *)PHYS_TO_VIRT(paddr), PAGE_SIZE);
		if (vm_map(p->vm, vaddr, (uint64_t)new_p, VM_USER | VM_READ | VM_WRITE) == 0) {
			pmm_free((void *)paddr);
			p->mem_charged += PAGE_SIZE;
			ret = 0;
		} else {
			pmm_free(new_p);
			cgroup_uncharge_mem(p->cgroup, PAGE_SIZE);
		}
	} else {
		ret = vm_map(p->vm, vaddr, paddr, VM_USER | VM_READ | VM_WRITE);
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
