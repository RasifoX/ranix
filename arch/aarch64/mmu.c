#include <kernel/mm/mmu.h>
#include <kernel/mm/vm.h>
#include <asm/mmu.h>

/* physical — VMA=LMA (in .bss.boot, before high-half section).
 * 48-bit VA / 4-level: L0 is the TTBR root. L0[0]→L1 (TTBR0, low 512GB) and
 * L0_high[256]→L1_high (TTBR1 kernel image + direct map at VIRT_OFFSET); the
 * existing L1 tables keep their layout one level down. */
__attribute__((section(".bss.boot"), aligned(4096))) uint64_t kernel_l0[512];
__attribute__((section(".bss.boot"), aligned(4096))) uint64_t kernel_l0_high[512];
__attribute__((section(".bss.boot"), aligned(4096))) uint64_t kernel_l1[512];
__attribute__((section(".bss.boot"), aligned(4096))) uint64_t kernel_l1_high[512];
__attribute__((section(".bss.boot"), aligned(4096))) static uint64_t l2[512];

#define VIRT_OFFSET_L0_IDX  256   /* (0xFFFF800000000000 >> 39) & 0x1FF */

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

	/* L0 roots over the existing L1 tables (4-level / 48-bit VA). TTBR0 keeps
	 * the identity map at slot 0; TTBR1 roots the kernel window at slot 256. */
	kernel_l0[0] = (uint64_t)kernel_l1 | PTE_TABLE | PTE_VALID;
	kernel_l0_high[VIRT_OFFSET_L0_IDX] = (uint64_t)kernel_l1_high | PTE_TABLE | PTE_VALID;

	enable_mmu((uint64_t)kernel_l0, (uint64_t)kernel_l0_high);
}

void mmu_init(void)
{
	vm_init_kernel((uint64_t)kernel_l0_high);
	vm_set_kernel_pt((uint64_t)kernel_l0);
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
	clean_page(kernel_l0);
	clean_page(kernel_l0_high);
	clean_page(kernel_l1);
	clean_page(kernel_l1_high);
	clean_page(l2);
	__asm__ volatile("dsb sy" ::: "memory");
}

/* Direct map covers L0_high[256] (boot kernel_l1_high, first 512 GB). To map
 * RAM past 512 GB we hand out more L1 tables, one per extra 512 GB slot, up to
 * L0_high[510] — i.e. ~127 TB; slot 511 stays reserved for ioremap. NOLOAD
 * .bss, so the pool costs nothing in the binary. */
#define DM_EXTRA_L1S 254   /* L0_high[257..510] */
static uint64_t dm_pool[DM_EXTRA_L1S][512] __attribute__((aligned(4096)));

/* PHYS_TO_VIRT(p) lands at L0_high index 256 + (p>>39), L1 index (p>>30)&0x1FF.
 * Boot fills only 0-8GB in slot 256; fill the rest up to max_phys with 1GB
 * blocks so all RAM is reachable before the buddy frees it. */
void arch_map_high_memory(uint64_t max_phys)
{
	int used = 0;   /* count of dm_pool tables touched, for the coherency clean */
	for (uint64_t phys = 8ULL << 30; phys < max_phys; phys += 1ULL << 30) {
		uint64_t l0i = VIRT_OFFSET_L0_IDX + (phys >> 39);
		if (l0i > 510) break;                         /* ioremap-region cap */
		uint64_t l1i = (phys >> 30) & 0x1FF;

		uint64_t *l1;
		if (l0i == VIRT_OFFSET_L0_IDX) {
			l1 = kernel_l1_high;
		} else if (kernel_l0_high[l0i] & PTE_VALID) {
			l1 = (uint64_t *)PHYS_TO_VIRT(kernel_l0_high[l0i] & ~0xFFFUL);
		} else {
			int slot = (int)(l0i - VIRT_OFFSET_L0_IDX - 1);
			l1 = dm_pool[slot];
			kernel_l0_high[l0i] = VIRT_TO_PHYS((uint64_t)l1) | PTE_TABLE | PTE_VALID;
			if (slot + 1 > used) used = slot + 1;
		}
		if (l1[l1i] & PTE_VALID) continue;
		l1[l1i] = phys | PTE_BLOCK | PTE_AF | PTE_VALID | PTE_ATTR_NORMAL;
	}

	/* Clean the extra L1 tables (written cacheably here) to PoC, mirroring
	 * mmu_clean_tables_poc for the boot tables, so a secondary's MMU-on walk
	 * sees these high-RAM entries. The L0_high page is cleaned by that routine. */
	for (int s = 0; s < used; s++)
		clean_page(dm_pool[s]);
	__asm__ volatile("dsb sy; isb" ::: "memory");
}
