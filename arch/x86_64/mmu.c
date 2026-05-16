#include <kernel/mm/mmu.h>
#include <kernel/mm/vm.h>
#include <kernel/lib/string.h>
#include <asm/mmu.h>

extern uint64_t pml4[];
extern uint64_t pdpt_dm[];

void mmu_init(void)
{
	vm_init_kernel((uint64_t)pml4);
	vm_set_kernel_pt((uint64_t)pml4);
}

void arch_map_high_memory(uint64_t max_phys)
{
	/* Use 1GB huge pages (PDPE1GB) — no PD allocation needed, no buddy dep. */
	for (uint64_t phys = 4ULL << 30; phys < max_phys; phys += 1ULL << 30) {
		uint64_t idx = phys >> 30;
		if (idx >= 512) break;
		if (pdpt_dm[idx] & 1) continue;
		pdpt_dm[idx] = phys | 0x83UL;  /* P + RW + PS(1GB) */
	}
}
