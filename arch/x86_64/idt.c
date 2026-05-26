#include <asm/idt.h>
#include <uapi/types.h>

struct idt_entry {
	uint16_t offset_lo;
	uint16_t selector;
	uint8_t  ist;
	uint8_t  flags;
	uint16_t offset_mid;
	uint32_t offset_hi;
	uint32_t reserved;
} __attribute__((packed));

struct idtr {
	uint16_t limit;
	uint64_t base;
} __attribute__((packed));

static struct idt_entry idt[256];

void idt_set_gate(uint8_t vec, void *handler, uint8_t flags)
{
	uint64_t addr = (uint64_t)handler;
	idt[vec].offset_lo  = addr & 0xFFFF;
	idt[vec].selector   = 0x08;   /* kernel code segment */
	idt[vec].ist        = 0;
	idt[vec].flags      = flags;
	idt[vec].offset_mid = (addr >> 16) & 0xFFFF;
	idt[vec].offset_hi  = (addr >> 32) & 0xFFFFFFFF;
	idt[vec].reserved   = 0;
}

/* exception stubs declared in exceptions_asm.S */
extern void exc_stub_0(void);  extern void exc_stub_1(void);
extern void exc_stub_2(void);  extern void exc_stub_3(void);
extern void exc_stub_4(void);  extern void exc_stub_5(void);
extern void exc_stub_6(void);  extern void exc_stub_7(void);
extern void exc_stub_8(void);  extern void exc_stub_9(void);
extern void exc_stub_10(void); extern void exc_stub_11(void);
extern void exc_stub_12(void); extern void exc_stub_13(void);
extern void exc_stub_14(void); extern void exc_stub_15(void);
extern void exc_stub_16(void); extern void exc_stub_17(void);
extern void exc_stub_18(void); extern void exc_stub_19(void);
extern void irq_stub_20(void); extern void irq_stub_21(void);
extern void irq_stub_22(void); extern void irq_stub_23(void);
extern void irq_stub_24(void); extern void irq_stub_25(void);
extern void irq_stub_26(void); extern void irq_stub_27(void);
extern void irq_stub_28(void); extern void irq_stub_29(void);
extern void irq_stub_2a(void); extern void irq_stub_2b(void);
extern void irq_stub_2c(void); extern void irq_stub_2d(void);
extern void irq_stub_2e(void); extern void irq_stub_2f(void);
extern void irq_stub_yield(void);
extern void irq_stub_syscall(void);
extern void irq_stub_ipi(void);

void idt_init(void)
{
	static void *exc_stubs[20] = {
		exc_stub_0,  exc_stub_1,  exc_stub_2,  exc_stub_3,
		exc_stub_4,  exc_stub_5,  exc_stub_6,  exc_stub_7,
		exc_stub_8,  exc_stub_9,  exc_stub_10, exc_stub_11,
		exc_stub_12, exc_stub_13, exc_stub_14, exc_stub_15,
		exc_stub_16, exc_stub_17, exc_stub_18, exc_stub_19,
	};

	for (int i = 0; i < 20; i++)
		idt_set_gate(i, exc_stubs[i], 0x8E);  /* present, ring0, interrupt gate */

	/* hardware IRQs from PIC */
	static void *irq_stubs[16] = {
		irq_stub_20, irq_stub_21, irq_stub_22, irq_stub_23,
		irq_stub_24, irq_stub_25, irq_stub_26, irq_stub_27,
		irq_stub_28, irq_stub_29, irq_stub_2a, irq_stub_2b,
		irq_stub_2c, irq_stub_2d, irq_stub_2e, irq_stub_2f,
	};

	for (int i = 0; i < 16; i++)
		idt_set_gate(0x20 + i, irq_stubs[i], 0x8E);

	idt_set_gate(VEC_YIELD,   irq_stub_yield,   0xEE);
	idt_set_gate(VEC_SYSCALL, irq_stub_syscall, 0xEE);
	idt_set_gate(0xF0,        irq_stub_ipi,     0x8E);  /* SMP reschedule IPI */

	struct idtr idtr = {
		.limit = sizeof(idt) - 1,
		.base  = (uint64_t)idt,
	};
	__asm__ volatile("lidt %0" :: "m"(idtr));
}

void idt_load(void)
{
	struct idtr idtr = {
		.limit = sizeof(idt) - 1,
		.base  = (uint64_t)idt,
	};
	__asm__ volatile("lidt %0" :: "m"(idtr));
}
