#include <kernel/mm/mmu.h>
#include <kernel/mm/vm.h>

extern uint64_t pml4[];

void mmu_init(void)
{
	vm_init_kernel((uint64_t)pml4);
	vm_set_kernel_pt((uint64_t)pml4);
}