#include <kernel/mm/mmu.h>
#include <kernel/mm/vm.h>

static uint64_t ram_limit = 0x3C000000UL;
static uint64_t fb_limit  = 0x40000000UL;

void mmu_set_ram_limit(uint64_t limit) { ram_limit = limit; }

void mmu_set_fb_region(uint64_t fb_start, uint64_t fb_end)
{
	ram_limit = fb_start;
	fb_limit  = fb_end;
}

__attribute__((aligned(4096))) uint64_t kernel_l1[512];
__attribute__((aligned(4096))) static uint64_t l2[512];

extern void enable_mmu(uint64_t table_addr);

void mmu_init(void)
{
	for (int i = 0; i < 512; i++) kernel_l1[i] = l2[i] = 0;

	kernel_l1[0] = (uint64_t)l2 | PTE_TABLE | PTE_VALID;

	for (int i = 0; i < 512; i++) {
		uint64_t addr = (uint64_t)i * 0x200000;
		uint64_t attr;

		if (addr < ram_limit)       attr = PTE_ATTR_NORMAL;
		else if (addr < fb_limit)   attr = PTE_ATTR_NORMAL_NC;
		else                        attr = PTE_ATTR_DEVICE;

		l2[i] = addr | PTE_BLOCK | PTE_AF | PTE_VALID | attr;
	}

	kernel_l1[1] = 0x40000000UL | PTE_BLOCK | PTE_AF | PTE_VALID | PTE_ATTR_DEVICE;
	kernel_l1[2] = 0x80000000UL | PTE_BLOCK | PTE_AF | PTE_VALID | PTE_ATTR_DEVICE;
	kernel_l1[3] = 0xC0000000UL | PTE_BLOCK | PTE_AF | PTE_VALID | PTE_ATTR_DEVICE;

	enable_mmu((uint64_t)kernel_l1);
	vm_set_kernel_pt((uint64_t)kernel_l1);
}
