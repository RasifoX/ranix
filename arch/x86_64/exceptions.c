#include <asm/context.h>
#include <asm/gdt.h>
#include <asm/idt.h>
#include <kernel/kprintf.h>
#include <kernel/mm/vm.h>
#include <kernel/proc.h>

void exceptions_init(void)
{
	gdt_init(0);
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
	extern int  ptrace_trap(void);
	extern void ptrace_apply_singlestep(arch_context_t *);

	if ((ctx->cs & 3) == 3) {
		process_t *cur = task_current();
		if (cur) cur->uctx = ctx;
	}

	if (ctx->vector == 14) {
		uint64_t cr2 = read_cr2();
		int is_write = (ctx->error_code & (1 << 1)) != 0;
		int is_present = (ctx->error_code & (1 << 0)) != 0;
		/* Handle both translation faults (not present) and permission faults (COW) */
		if (!is_present || is_write) {
			if (vm_handle_fault(cr2, is_write) == 0) {
				ptrace_apply_singlestep(ctx);
				return;
			}
		}
	}

	/* Debug traps: single-step (#DB) or breakpoint (#BP, int3). A traced ring-3
	 * process reports a SIGTRAP stop; a stray #DB just resumes (don't panic).
	 * TF is re-armed by ptrace_apply_singlestep on the way back to user. */
	if (ctx->vector == 1) {
		if ((ctx->cs & 3) == 3) ptrace_trap();
		ptrace_apply_singlestep(ctx);
		return;
	}
	if (ctx->vector == 3 && (ctx->cs & 3) == 3) {
		if (ptrace_trap()) { ptrace_apply_singlestep(ctx); return; }
	}

	/* Ring-3 fault — kill the process, don't panic the kernel */
	if ((ctx->cs & 3) == 3) {
		process_t *p = task_current();
		kprintf("segfault: pid=%d (%s) RIP=0x%lx vec=%d err=0x%lx\n",
		        (int)p->pid, p->name, ctx->rip, (int)ctx->vector, ctx->error_code);
		if (ctx->vector == 14)
			kprintf("  CR2=0x%lx\n", read_cr2());
		kprintf("  rax=0x%lx rbx=0x%lx rbp=0x%lx rsp=0x%lx\n",
		        ctx->rax, ctx->rbx, ctx->rbp, ctx->rsp);
		extern void coredump(process_t *, int, arch_context_t *);
		coredump(p, SIGSEGV, ctx);
		p->killed_by_sig = 1;
		p->exit_code     = SIGSEGV;
		task_exit();
		__builtin_unreachable();
	}

	panic_report(exc_name(ctx->vector), ctx);
}
