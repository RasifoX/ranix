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

struct tss_desc {
	uint16_t limit_lo;
	uint16_t base_lo;
	uint8_t  base_mid;
	uint8_t  flags1;
	uint8_t  flags2;
	uint8_t  base_hi;
	uint32_t base_upper;
	uint32_t reserved;
} __attribute__((packed));

struct gdtr {
	uint16_t limit;
	uint64_t base;
} __attribute__((packed));

struct tss64 {
	uint32_t reserved0;
	uint64_t rsp0;
	uint64_t rsp1;
	uint64_t rsp2;
	uint64_t reserved1;
	uint64_t ist[7];
	uint64_t reserved2;
	uint16_t reserved3;
	uint16_t iopb;
} __attribute__((packed));

/* GDT layout: 5 regular + 1 TSS (16-byte) = 56 bytes */
static struct {
	struct gdt_entry entries[5];
	struct tss_desc  tssd;
} __attribute__((packed)) gdt;

static struct tss64 tss;
static struct gdtr  gdtr;

uint64_t current_kstack_top = 0;

void tss_set_rsp0(uint64_t rsp0)
{
	tss.rsp0 = rsp0;
	current_kstack_top = rsp0;
}

void gdt_init(void)
{
	/* null */
	gdt.entries[0] = (struct gdt_entry){ 0 };

	/* kernel code: present, ring0, code exec/read, L=1 */
	gdt.entries[1] = (struct gdt_entry){ .access = 0x9A, .gran = 0x20 };

	/* kernel data: present, ring0, data read/write */
	gdt.entries[2] = (struct gdt_entry){ .access = 0x92 };

	/* user code: present, ring3, code exec/read, L=1 */
	gdt.entries[3] = (struct gdt_entry){ .access = 0xFA, .gran = 0x20 };

	/* user data: present, ring3, data read/write */
	gdt.entries[4] = (struct gdt_entry){ .access = 0xF2 };

	/* 64-bit TSS descriptor */
	uint64_t base = (uint64_t)&tss;
	uint32_t lim  = sizeof(tss) - 1;
	gdt.tssd.limit_lo   = lim & 0xFFFF;
	gdt.tssd.base_lo    = base & 0xFFFF;
	gdt.tssd.base_mid   = (base >> 16) & 0xFF;
	gdt.tssd.flags1     = 0x89;   /* P=1, DPL=0, type=9 (TSS available) */
	gdt.tssd.flags2     = (uint8_t)((lim >> 16) & 0x0F);
	gdt.tssd.base_hi    = (base >> 24) & 0xFF;
	gdt.tssd.base_upper = (base >> 32) & 0xFFFFFFFF;
	gdt.tssd.reserved   = 0;

	tss.iopb = sizeof(tss);

	gdtr.limit = sizeof(gdt) - 1;
	gdtr.base  = (uint64_t)&gdt;

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
		"mov $0x28, %%ax\n"
		"ltr %%ax\n"
		:: "m"(gdtr) : "rax", "memory"
	);
}
