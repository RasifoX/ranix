#include <asm/gdt.h>
#include <kernel/smp.h>
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
} __attribute__((packed)) cpu_gdt[MAX_CPUS];

static struct tss64 cpu_tss[MAX_CPUS];
static struct gdtr  cpu_gdtr[MAX_CPUS];

void tss_set_rsp0(uint64_t rsp0)
{
	uint64_t gs_base;
	__asm__ volatile("movq %%gs:0, %0" : "=r"(gs_base));
	/* gs:0 is cpu_context_t->self; cpu_id is at offset 32 */
	uint64_t cpu_id;
	__asm__ volatile("movq %%gs:32, %0" : "=r"(cpu_id));
	if (cpu_id < MAX_CPUS)
		cpu_tss[cpu_id].rsp0 = rsp0;
}

void gdt_init(int cpu_id)
{
	if ((unsigned)cpu_id >= MAX_CPUS) cpu_id = 0;

	cpu_gdt[cpu_id].entries[0] = (struct gdt_entry){ 0 };
	cpu_gdt[cpu_id].entries[1] = (struct gdt_entry){ .access = 0x9A, .gran = 0x20 };
	cpu_gdt[cpu_id].entries[2] = (struct gdt_entry){ .access = 0x92 };
	cpu_gdt[cpu_id].entries[3] = (struct gdt_entry){ .access = 0xFA, .gran = 0x20 };
	cpu_gdt[cpu_id].entries[4] = (struct gdt_entry){ .access = 0xF2 };

	uint64_t base = (uint64_t)&cpu_tss[cpu_id];
	uint32_t lim  = sizeof(struct tss64) - 1;
	cpu_gdt[cpu_id].tssd.limit_lo   = lim & 0xFFFF;
	cpu_gdt[cpu_id].tssd.base_lo    = base & 0xFFFF;
	cpu_gdt[cpu_id].tssd.base_mid   = (base >> 16) & 0xFF;
	cpu_gdt[cpu_id].tssd.flags1     = 0x89;
	cpu_gdt[cpu_id].tssd.flags2     = (uint8_t)((lim >> 16) & 0x0F);
	cpu_gdt[cpu_id].tssd.base_hi    = (base >> 24) & 0xFF;
	cpu_gdt[cpu_id].tssd.base_upper = (base >> 32) & 0xFFFFFFFF;
	cpu_gdt[cpu_id].tssd.reserved   = 0;

	cpu_tss[cpu_id].iopb = sizeof(struct tss64);

	cpu_gdtr[cpu_id].limit = sizeof(cpu_gdt[0]) - 1;
	cpu_gdtr[cpu_id].base  = (uint64_t)&cpu_gdt[cpu_id];

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
		:: "m"(cpu_gdtr[cpu_id]) : "rax", "memory"
	);
}
