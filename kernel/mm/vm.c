#include <kernel/mm/vm.h>
#include <kernel/mm/buddy.h>
#include <kernel/mm/pmm.h>
#include <kernel/mm/mmu.h>
#include <kernel/hal.h>
#include <kernel/lib/string.h>
#include <kernel/kprintf.h>
#include <kernel/sched.h>
#include <asm/mmu.h>

static uint64_t kernel_pt_phys;
static vm_space_t _kernel_vm_space;
vm_space_t *kernel_vm_space = &_kernel_vm_space;

#ifdef __aarch64__
static uint64_t ioremap_vaddr = 0xFFFFFFC000000000UL;
#else
static uint64_t ioremap_vaddr = 0xFFFFFFFFC0000000UL;
#endif

void vm_set_kernel_pt(uint64_t phys)
{
	kernel_pt_phys = phys;
}

void vm_init_kernel(uint64_t root_phys)
{
	kernel_pt_phys = root_phys;
	_kernel_vm_space.root = (uint64_t *)PHYS_TO_VIRT(root_phys);
}

void *ioremap(uint64_t phys_addr, size_t size, uint32_t flags)
{
	uint64_t aligned_phys = phys_addr & PAGE_MASK;
	uint64_t offset       = phys_addr & ~PAGE_MASK;
	uint64_t aligned_size = (size + offset + PAGE_SIZE - 1) & PAGE_MASK;

	uint64_t vaddr = ioremap_vaddr;
	ioremap_vaddr += aligned_size;

	for (uint64_t i = 0; i < aligned_size; i += PAGE_SIZE) {
		vm_map(kernel_vm_space, vaddr + i, aligned_phys + i, flags);
	}

	return (void *)(vaddr + offset);
}

int vm_handle_fault(uint64_t vaddr, int is_write)
{
	if (!is_write) return -1;
	
	vaddr &= PAGE_MASK;
	process_t *p = task_current();
	if (!p || !p->vm) return -1;

	/* Find physical address. 
	 * Note: This assumes vm_map can be used to re-map.
	 * We need a way to get the physical address of a virtual address.
	 * Let's add arch_vm_get_paddr. */
	extern uint64_t arch_vm_get_paddr(uint64_t *root, uint64_t vaddr);
	uint64_t paddr = arch_vm_get_paddr(p->vm->root, vaddr);
	if (!paddr) return -1;

	page_t *pg = paddr_to_page(paddr);
	if (!pg) return -1;

	if (pg->refcount > 1) {
		/* COW: Duplicate the page */
		void *new_p = pmm_alloc();
		if (!new_p) return -1;
		
		memcpy((void *)PHYS_TO_VIRT(new_p), (void *)PHYS_TO_VIRT(paddr), PAGE_SIZE);
		
		/* Map new page as Writable */
		vm_map(p->vm, vaddr, (uint64_t)new_p, VM_USER | VM_READ | VM_WRITE);
		
		/* Release old page reference */
		pmm_free((void *)paddr);
	} else {
		/* Refcount == 1: Just mark as Writable */
		vm_map(p->vm, vaddr, paddr, VM_USER | VM_READ | VM_WRITE);
	}

	return 0;
}

void vm_activate(void)
{
	task_t *t = task_current();
	if (t->vm) {
		arch_set_user_kstack((uint64_t)t->kstack + KSTACK_SIZE);
		arch_vm_switch((uint64_t)t->vm->root);
	} else if (kernel_pt_phys) {
		arch_vm_switch(kernel_pt_phys);
	}
}
