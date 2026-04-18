#include <kernel/hal.h>
#include <kernel/lib/string.h>

void arch_halt(void)  { __asm__ volatile("wfi"); }
void arch_yield(void) { __asm__ volatile("svc #0"); }

void arch_context_init(arch_context_t *ctx, void (*entry)(void), uint64_t stack_top)
{
	memset(ctx, 0, sizeof(*ctx));
	ctx->elr  = (uint64_t)entry;
	ctx->spsr = ARCH_SPSR_KERNEL_TASK;
	ctx->sp   = stack_top;
}

void arch_context_init_user(arch_context_t *ctx, uint64_t entry, uint64_t ustack_top)
{
	memset(ctx, 0, sizeof(*ctx));
	ctx->elr  = entry;
	ctx->spsr = ARCH_SPSR_USER_TASK;  /* EL0t */
	ctx->sp   = ustack_top;
}

void arch_set_user_kstack(uint64_t kstack_top)
{
	(void)kstack_top;
	/* SP_EL1 is updated in restore_trapframe before every eret. */
}
