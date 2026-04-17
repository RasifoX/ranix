#include <asm/gdt.h>
#include <uapi/types.h>

struct gdt_entry {
	uint16_t limit_lo;
	uint16_t base_lo;
	uint8_t  base_mid;
	uint8_t  access;
	uint8_t  gran;
	uint8_t  base_hi;
} __attribute__((packed));

struct gdtr {
	uint16_t limit;
	uint64_t base;
} __attribute__((packed));

static struct gdt_entry gdt[3];

void gdt_init(void)
{
	/* null descriptor */
	gdt[0] = (struct gdt_entry){ 0 };

	/* 64-bit kernel code: base=0, limit=0, access=0x9A, gran=0x20 */
	gdt[1].limit_lo = 0;
	gdt[1].base_lo  = 0;
	gdt[1].base_mid = 0;
	gdt[1].access   = 0x9A;   /* present, ring0, code, execute/read */
	gdt[1].gran     = 0x20;   /* long mode (L bit) */
	gdt[1].base_hi  = 0;

	/* 64-bit kernel data: base=0, limit=0, access=0x92 */
	gdt[2].limit_lo = 0;
	gdt[2].base_lo  = 0;
	gdt[2].base_mid = 0;
	gdt[2].access   = 0x92;   /* present, ring0, data, read/write */
	gdt[2].gran     = 0;
	gdt[2].base_hi  = 0;

	struct gdtr gdtr = {
		.limit = sizeof(gdt) - 1,
		.base  = (uint64_t)gdt,
	};

	__asm__ volatile(
		"lgdt %0\n"
		"mov $0x10, %%ax\n"
		"mov %%ax, %%ds\n"
		"mov %%ax, %%es\n"
		"mov %%ax, %%ss\n"
		"pushq $0x08\n"
		"lea 1f(%%rip), %%rax\n"
		"pushq %%rax\n"
		"lretq\n"
		"1:\n"
		:: "m"(gdtr) : "rax", "memory"
	);
}
