#include <kernel/hal.h>
#include <kernel/lib/string.h>
#include <kernel/proc.h>

static cpu_context_t early_cpu_ctx;

void arch_halt(void)  { __asm__ volatile("wfi"); }
void arch_yield(void) { __asm__ volatile("svc #0"); }

void arch_cpu_init(void)
{
	/* Use early context until sched_init sets up proper per-CPU areas */
	__asm__ volatile("msr tpidr_el1, %0" :: "r"(&early_cpu_ctx));

	/* enable FP/SIMD access */
	uint64_t cpacr;
	__asm__ volatile("mrs %0, cpacr_el1" : "=r"(cpacr));
	cpacr |= (0x3ULL << 20);
	__asm__ volatile("msr cpacr_el1, %0" :: "r"(cpacr));
	__asm__ volatile("isb");
}

cpu_context_t *arch_get_cpu_ctx(void)
{
	cpu_context_t *ctx;
	__asm__ volatile("mrs %0, tpidr_el1" : "=r"(ctx));
	return ctx;
}

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
}