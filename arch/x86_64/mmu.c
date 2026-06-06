#include <kernel/mm/mmu.h>
#include <kernel/mm/vm.h>
#include <kernel/lib/string.h>
#include <asm/mmu.h>

extern uint64_t pml4[];
extern uint64_t pml5[];
extern uint64_t pdpt_dm[];
extern uint8_t  la57_enabled;

void mmu_init(void)
{
	/* Read the boot LA57 flag while the identity map is still live, and cache
	 * it in the high half for the fault-path code (see arch_vm_set_la57). */
	extern void arch_vm_set_la57(int v);
	int la57 = la57_enabled;
	arch_vm_set_la57(la57);

	/* Kernel address space root the CPU loads into CR3: PML5 under 5-level. */
	uint64_t root = la57 ? (uint64_t)pml5 : (uint64_t)pml4;
	vm_init_kernel(root);
	vm_set_kernel_pt(root);

	extern void kprintf(const char *, ...);
	kprintf("paging: %s\n", la57 ? "5-level (LA57, 128 PB VA)" : "4-level (256 TB VA)");
}

/* Direct map covers PML4[256] (boot pdpt_dm, first 512 GB). To map RAM past
 * 512 GB we hand out more PDPTs from this pool, one per extra 512 GB slot, up
 * to PML4[510] (leaving PML4[511] for kernel text / ioremap) — i.e. ~127 TB in
 * 4-level paging. Beyond that needs 5-level (LA57): a separate, deeper rework.
 * NOLOAD .bss, so the pool costs nothing in the binary. */
#define DM_EXTRA_PDPTS 254   /* PML4[257..510] */
static uint64_t dm_pool[DM_EXTRA_PDPTS][512] __attribute__((aligned(4096)));

void arch_map_high_memory(uint64_t max_phys)
{
	/* 1GB huge pages (PDPE1GB) — no PD allocation, no buddy dependency. */
	for (uint64_t phys = 4ULL << 30; phys < max_phys; phys += 1ULL << 30) {
		uint64_t pml4i = 256 + (phys >> 39);          /* 512 GB per PML4 slot */
		if (pml4i > 510) break;                       /* VA / kernel-region cap */
		uint64_t pdpti = (phys >> 30) & 0x1FF;

		uint64_t *pdpt;
		if (pml4i == 256) {
			pdpt = pdpt_dm;                           /* boot-provided PDPT */
		} else if (pml4[pml4i] & 1) {
			pdpt = (uint64_t *)PHYS_TO_VIRT(pml4[pml4i] & ~0xFFFUL);
		} else {
			pdpt = dm_pool[pml4i - 257];
			pml4[pml4i] = VIRT_TO_PHYS((uint64_t)pdpt) | 0x3UL;  /* P + RW */
		}
		if (pdpt[pdpti] & 1) continue;
		pdpt[pdpti] = phys | 0x83UL | (1UL << 63);    /* P + RW + PS(1GB) + NX (data only) */
	}
}
