#include <kernel/hal.h>
#include <kernel/lib/string.h>
#include <asm/gdt.h>

void arch_set_user_kstack(uint64_t kstack_top)
{
	tss_set_rsp0(kstack_top);
}

void arch_halt(void)  { __asm__ volatile("hlt"); }
void arch_yield(void) { __asm__ volatile("int $0x40"); }

void arch_context_init(arch_context_t *ctx, void (*entry)(void), uint64_t stack_top)
{
	memset(ctx, 0, sizeof(*ctx));
	ctx->rip    = (uint64_t)entry;
	ctx->cs     = GDT_KCODE;
	ctx->rflags = ARCH_SPSR_KERNEL_TASK;
	ctx->rsp    = stack_top;
	ctx->ss     = GDT_KDATA;
}

void arch_context_init_user(arch_context_t *ctx, uint64_t entry, uint64_t ustack_top)
{
	memset(ctx, 0, sizeof(*ctx));
	ctx->rip    = entry;
	ctx->cs     = GDT_USER_CODE;   /* ring3 code: 0x1B */
	ctx->rflags = ARCH_SPSR_USER_TASK;
	ctx->rsp    = ustack_top;
	ctx->ss     = GDT_USER_DATA;   /* ring3 data: 0x23 */
}
