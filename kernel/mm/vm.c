#include <kernel/mm/vm.h>
#include <kernel/mm/buddy.h>
#include <kernel/mm/mmu.h>
#include <kernel/lib/string.h>
#include <kernel/kprintf.h>
#include <kernel/sched.h>

static uint64_t kernel_pt_phys;

void vm_set_kernel_pt(uint64_t phys)
{
	kernel_pt_phys = phys;
}

void vm_activate(void)
{
	task_t *t = task_current();
	uint64_t pt = t->vm ? (uint64_t)t->vm->root : kernel_pt_phys;
	if (pt)
		arch_vm_switch(pt);
}
