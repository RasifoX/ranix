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
extern void irq_stub_0(void);  /* PIT timer */
extern void irq_stub_yield(void);

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

	idt_set_gate(VEC_TIMER, irq_stub_0,     0x8E);
	idt_set_gate(VEC_YIELD, irq_stub_yield, 0xEE);  /* ring0+3 callable */

	struct idtr idtr = {
		.limit = sizeof(idt) - 1,
		.base  = (uint64_t)idt,
	};
	__asm__ volatile("lidt %0" :: "m"(idtr));
}
