#include <asm/context.h>
#include <asm/gdt.h>
#include <asm/idt.h>
#include <kernel/kprintf.h>

void exceptions_init(void)
{
	gdt_init();
	idt_init();
}

static const char *exc_name(uint64_t vec)
{
	static const char *names[] = {
		"divide error", "debug", "NMI", "breakpoint",
		"overflow", "bound range", "invalid opcode", "device not available",
		"double fault", "coprocessor segment", "invalid TSS", "segment not present",
		"stack fault", "general protection", "page fault", "(reserved)",
		"x87 error", "alignment check", "machine check", "SIMD error",
	};
	if (vec < 20) return names[vec];
	return "unknown";
}

static uint64_t read_cr2(void)
{
	uint64_t v;
	__asm__ volatile("mov %%cr2, %0" : "=r"(v));
	return v;
}

void exc_dispatch(arch_context_t *ctx)
{
	kprintf("\n--- kernel fault ---\n");
	kprintf("type:  #%d %s\n", (int)ctx->vector, exc_name(ctx->vector));
	kprintf("RIP:   0x%x\n", (unsigned int)ctx->rip);
	kprintf("ERR:   0x%x\n", (unsigned int)ctx->error_code);
	if (ctx->vector == 14)
		kprintf("CR2:   0x%x\n", (unsigned int)read_cr2());
	kprintf("RFLAGS:0x%x\n", (unsigned int)ctx->rflags);
	kprintf("RSP:   0x%x\n", (unsigned int)ctx->rsp);
	kpanic("unhandled exception");
}
