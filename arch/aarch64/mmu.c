#include <kernel/mm/mmu.h>
#include <kernel/mm/vm.h>
#include <asm/mmu.h>

/* physical — VMA=LMA (in .bss.boot, before high-half section) */
__attribute__((section(".bss.boot"), aligned(4096))) uint64_t kernel_l1[512];
__attribute__((section(".bss.boot"), aligned(4096))) static uint64_t kernel_l1_high[512];
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

uint64_t arch_direct_map_boot_limit(void) { return (uint64_t)-1; }

void arch_map_high_memory(uint64_t max_phys)
{
	(void)max_phys;
}