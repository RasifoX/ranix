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
