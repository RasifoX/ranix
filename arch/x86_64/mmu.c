#include <kernel/mm/mmu.h>
#include <kernel/mm/vm.h>
#include <kernel/mm/buddy.h>
#include <kernel/lib/string.h>
#include <kernel/kprintf.h>
#include <asm/mmu.h>

extern uint64_t pml4[];
extern uint64_t pdpt_dm[];

void mmu_init(void)
{
	vm_init_kernel((uint64_t)pml4);
	vm_set_kernel_pt((uint64_t)pml4);
}

uint64_t arch_direct_map_boot_limit(void) { return 4ULL << 30; }

void arch_map_high_memory(uint64_t max_phys)
{
	/* boot.S already mapped 0..4GB in pdpt_dm[0..3]; cover the rest */
	for (uint64_t phys = 4ULL << 30; phys < max_phys; phys += 1ULL << 30) {
		uint64_t idx = phys >> 30;
		if (idx >= 512) break;
		if (pdpt_dm[idx] & 1) continue;

		void *pd_phys = buddy_alloc(0);
		if (!pd_phys) { kprintf("mmu: OOM at phys 0x%lx\n", phys); break; }
		uint64_t *pd = (uint64_t *)PHYS_TO_VIRT((uint64_t)pd_phys);
		memset(pd, 0, PAGE_SIZE);
		for (int i = 0; i < 512; i++)
			pd[i] = (phys + (uint64_t)i * 0x200000) | 0x83UL;
		pdpt_dm[idx] = (uint64_t)pd_phys | 0x3UL;
	}
}