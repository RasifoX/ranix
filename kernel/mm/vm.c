#include <kernel/mm/vm.h>
#include <kernel/mm/buddy.h>
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
