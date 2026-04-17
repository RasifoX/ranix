#include <kernel/mm/mmu.h>
#include <kernel/mm/vm.h>

extern uint64_t pml4[];

void mmu_set_ram_limit(uint64_t limit)         { (void)limit; }
void mmu_set_fb_region(uint64_t a, uint64_t b) { (void)a; (void)b; }

void mmu_init(void)
{
	vm_set_kernel_pt((uint64_t)pml4);
}
