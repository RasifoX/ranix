#include <asm/context.h>
#include <asm/gdt.h>
#include <asm/idt.h>
#include <kernel/kprintf.h>
#include <kernel/mm/vm.h>
#include <kernel/proc.h>

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
	if (ctx->vector == 14) {
		uint64_t cr2 = read_cr2();
		int is_write = (ctx->error_code & (1 << 1)) != 0;
		int is_present = (ctx->error_code & (1 << 0)) != 0;
		/* Handle both translation faults (not present) and permission faults (COW) */
		if (!is_present || is_write) {
			if (vm_handle_fault(cr2, is_write) == 0)
				return;
		}
	}

	/* Ring-3 fault — kill the process, don't panic the kernel */
	if ((ctx->cs & 3) == 3) {
		process_t *p = task_current();
		kprintf("segfault: pid=%d (%s) RIP=0x%lx vec=%d err=0x%lx\n",
		        (int)p->pid, p->name, ctx->rip, (int)ctx->vector, ctx->error_code);
		if (ctx->vector == 14)
			kprintf("  CR2=0x%lx\n", read_cr2());
		task_exit();
		__builtin_unreachable();
	}

	kprintf("\n--- kernel fault ---\n");
	kprintf("type:  #%d %s\n", (int)ctx->vector, exc_name(ctx->vector));
	kprintf("RIP:   0x%016lx\n", ctx->rip);
	kprintf("RSP:   0x%016lx\n", ctx->rsp);
	kprintf("RFLAGS:0x%08lx\n", ctx->rflags);
	kprintf("ERR:   0x%lx\n", ctx->error_code);
	if (ctx->vector == 14)
		kprintf("CR2:   0x%016lx\n", read_cr2());
	kpanic("unhandled exception");
}
