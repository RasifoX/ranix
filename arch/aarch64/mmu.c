#include <kernel/mm/mmu.h>
#include <kernel/mm/vm.h>
#include <asm/mmu.h>

/* physical — VMA=LMA (in .bss.boot, before high-half section) */
__attribute__((section(".bss.boot"), aligned(4096))) uint64_t kernel_l1[512];
__attribute__((section(".bss.boot"), aligned(4096))) uint64_t kernel_l1_high[512];
__attribute__((section(".bss.boot"), aligned(4096))) static uint64_t l2[512];

extern void enable_mmu(uint64_t ttbr0_phys, uint64_t ttbr1_phys);

__attribute__((section(".text.boot")))
void mmu_tables_init(void)
{
	int i;
	for (i = 0; i < 512; i++) {
		kernel_l1[i] = kernel_l1_high[i] = 0;
		l2[i] = 0;
	}

	uint64_t phys_pc;
	__asm__ volatile("adr %0, ." : "=r"(phys_pc));
	int is_virt = (phys_pc >= 0x40000000UL);

	/* Map 0-1GB via L2 table so vm_map can split it for userspace */
	for (i = 0; i < 512; i++) {
		uint64_t _a = (uint64_t)i * 0x200000;
		uint64_t _attr = PTE_ATTR_NORMAL;
		if (is_virt) _attr = PTE_ATTR_DEVICE;

		uint64_t entry = _a | PTE_BLOCK | PTE_AF | PTE_VALID | _attr;
		l2[i] = entry;
	}

	/* Share the L2 table between identity and high-half mappings */
	kernel_l1[0] = (uint64_t)l2 | PTE_TABLE | PTE_VALID;
	kernel_l1_high[0] = (uint64_t)l2 | PTE_TABLE | PTE_VALID;

	/* Map 1GB-8GB using 1GB blocks */
	for (i = 1; i < 8; i++) {
		uint64_t attr = PTE_ATTR_NORMAL;
		if (!is_virt && i == 3) attr = PTE_ATTR_DEVICE;

		uint64_t entry = ((uint64_t)i << 30) | PTE_BLOCK | PTE_AF | PTE_VALID | attr;
		kernel_l1[i] = entry;
		kernel_l1_high[i] = entry;
	}

	enable_mmu((uint64_t)kernel_l1, (uint64_t)kernel_l1_high);
}

void mmu_init(void)
{
	vm_init_kernel((uint64_t)kernel_l1_high);
	vm_set_kernel_pt((uint64_t)kernel_l1);
}

/* Clean the boot page tables to the point of coherency so a secondary CPU,
 * walking them with caches+MMU about to come on, sees the latest entries
 * even if we touched them cacheably after early boot. */
static void clean_page(uint64_t *page)
{
	for (uint64_t a = (uint64_t)page; a < (uint64_t)page + 4096; a += 64)
		__asm__ volatile("dc cvac, %0" :: "r"(a) : "memory");
}

void mmu_clean_tables_poc(void)
{
	clean_page(kernel_l1);
	clean_page(kernel_l1_high);
	clean_page(l2);
	__asm__ volatile("dsb sy" ::: "memory");
}

/* The kernel direct map is TTBR1 (kernel_l1_high): PHYS_TO_VIRT(p) lands at
 * its L1 index p>>30. Boot fills only 0-8GB; fill the rest up to max_phys with
 * 1GB blocks so all RAM (to the 39-bit / 512GB TTBR1 window) is reachable.
 * Past 512GB needs a 4th level (48-bit VA) — a separate rework. */
void arch_map_high_memory(uint64_t max_phys)
{
	uint64_t last = (max_phys - 1) >> 30;
	if (last > 511) last = 511;
	for (uint64_t idx = 8; idx <= last; idx++) {
		if (kernel_l1_high[idx] & PTE_VALID) continue;
		kernel_l1_high[idx] = (idx << 30) | PTE_BLOCK | PTE_AF | PTE_VALID | PTE_ATTR_NORMAL;
	}
	__asm__ volatile("dsb ishst; isb" ::: "memory");
}