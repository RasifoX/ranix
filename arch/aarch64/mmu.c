#include <kernel/mm/mmu.h>
#include <kernel/mm/vm.h>
#include <asm/mmu.h>

static uint64_t ram_limit = 0x3C000000UL;
static uint64_t fb_limit  = 0x40000000UL;

void mmu_set_ram_limit(uint64_t limit) { ram_limit = limit; }

void mmu_set_fb_region(uint64_t fb_start, uint64_t fb_end)
{
	ram_limit = fb_start;
	fb_limit  = fb_end;
}

/* physical — VMA=LMA (in .bss.boot, before high-half section) */
__attribute__((section(".bss.boot"), aligned(4096))) uint64_t kernel_l1[512];
__attribute__((section(".bss.boot"), aligned(4096))) static uint64_t l2[512];
__attribute__((section(".bss.boot"), aligned(4096))) static uint64_t kernel_l1_high[512];
__attribute__((section(".bss.boot"), aligned(4096))) static uint64_t l2_high[512];

extern void enable_mmu(uint64_t ttbr0_phys, uint64_t ttbr1_phys);

#define FILL_L2(l2t, l2t_hi, rl, fl)					\
	do {								\
		for (int _i = 0; _i < 512; _i++) {			\
			uint64_t _a = (uint64_t)_i * 0x200000;		\
			uint64_t _attr;					\
			if (_a < (rl))        _attr = PTE_ATTR_NORMAL;	\
			else if (_a < (fl))   _attr = PTE_ATTR_NORMAL_NC;\
			else                  _attr = PTE_ATTR_DEVICE;	\
			(l2t)[_i]    = _a | PTE_BLOCK | PTE_AF | PTE_VALID | _attr;\
			(l2t_hi)[_i] = (l2t)[_i];			\
		}							\
	} while (0)

__attribute__((section(".text.boot")))
void mmu_tables_init(void)
{
	int i;
	for (i = 0; i < 512; i++)
		kernel_l1[i] = kernel_l1_high[i] = l2[i] = l2_high[i] = 0;

	kernel_l1[0] = (uint64_t)l2      | PTE_TABLE | PTE_VALID;
	kernel_l1[1] = 0x40000000UL | PTE_BLOCK | PTE_AF | PTE_VALID | PTE_ATTR_DEVICE;
	kernel_l1[2] = 0x80000000UL | PTE_BLOCK | PTE_AF | PTE_VALID | PTE_ATTR_DEVICE;
	kernel_l1[3] = 0xC0000000UL | PTE_BLOCK | PTE_AF | PTE_VALID | PTE_ATTR_DEVICE;

	kernel_l1_high[0] = (uint64_t)l2_high | PTE_TABLE | PTE_VALID;
	kernel_l1_high[1] = 0x40000000UL | PTE_BLOCK | PTE_AF | PTE_VALID | PTE_ATTR_DEVICE;
	kernel_l1_high[2] = 0x80000000UL | PTE_BLOCK | PTE_AF | PTE_VALID | PTE_ATTR_DEVICE;
	kernel_l1_high[3] = 0xC0000000UL | PTE_BLOCK | PTE_AF | PTE_VALID | PTE_ATTR_DEVICE;

	/* conservative defaults — mmu_init rebuilds with FDT limits */
	FILL_L2(l2, l2_high, 0x3C000000UL, 0x40000000UL);

	enable_mmu((uint64_t)kernel_l1, (uint64_t)kernel_l1_high);
}

void mmu_init(void)
{
	/* rebuild L2 entries with FDT-derived limits */
	FILL_L2(l2, l2_high, ram_limit, fb_limit);
	__asm__ volatile("dsb sy; tlbi vmalle1is; dsb sy; isb" ::: "memory");

	vm_set_kernel_pt((uint64_t)kernel_l1);
}
