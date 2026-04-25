#include <kernel/hal.h>
#include <kernel/lib/string.h>
#include <asm/gdt.h>
#include <kernel/proc.h>

extern void syscall_entry(void);

static cpu_context_t early_cpu_ctx;

void arch_set_user_kstack(uint64_t kstack_top)
{
	tss_set_rsp0(kstack_top);
}

void arch_halt(void)  { __asm__ volatile("hlt"); }
void arch_yield(void) { __asm__ volatile("int $0x40"); }

cpu_context_t *arch_get_cpu_ctx(void)
{
	cpu_context_t *ctx;
	__asm__ volatile("movq %%gs:0, %0" : "=r"(ctx));
	return ctx;
}

static void arch_syscall_init(void)
{
	early_cpu_ctx.self    = &early_cpu_ctx;
	early_cpu_ctx.current = NULL;
	early_cpu_ctx.cpu_id  = 0;
	early_cpu_ctx.magic   = 0xCAFEBABE;
	
	uint64_t gs_base = (uint64_t)&early_cpu_ctx;
	__asm__ volatile(
		"movl $0xC0000101, %%ecx\n"
		"movq %0, %%rax\n"
		"movq %%rax, %%rdx\n"
		"shrq $32, %%rdx\n"
		"wrmsr\n"
		:: "r"(gs_base) : "rax", "ecx", "edx"
	);

	/* EFER.SCE — enable syscall/sysret */
	__asm__ volatile(
		"movl $0xC0000080, %%ecx\n"
		"rdmsr\n"
		"orl  $1, %%eax\n"
		"wrmsr\n"
		::: "eax", "ecx", "edx"
	);

	/* STAR */
	uint64_t star = (0x18ULL << 48) | (0x08ULL << 32);
	__asm__ volatile(
		"movl $0xC0000081, %%ecx\n"
		"movl %%eax, %%eax\n"
		"movq %0, %%rax\n"
		"movq %%rax, %%rdx\n"
		"shrq $32, %%rdx\n"
		"wrmsr\n"
		:: "r"(star) : "rax", "ecx", "edx"
	);

	/* LSTAR */
	uint64_t lstar = (uint64_t)syscall_entry;
	__asm__ volatile(
		"movl $0xC0000082, %%ecx\n"
		"movq %0, %%rax\n"
		"movq %%rax, %%rdx\n"
		"shrq $32, %%rdx\n"
		"wrmsr\n"
		:: "r"(lstar) : "rax", "ecx", "edx"
	);

	/* SFMASK */
	__asm__ volatile(
		"movl $0xC0000084, %%ecx\n"
		"movl $0x200, %%eax\n"
		"xorl %%edx, %%edx\n"
		"wrmsr\n"
		::: "eax", "ecx", "edx"
	);
}

void arch_cpu_init(void)
{
	/* enable SSE and FXSAVE */
	uint64_t cr0, cr4;
	__asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
	cr0 &= ~(1ULL << 2);
	cr0 |= (1ULL << 1);
	__asm__ volatile("mov %0, %%cr0" :: "r"(cr0));

	__asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
	cr4 |= (1ULL << 9);
	cr4 |= (1ULL << 10);
	__asm__ volatile("mov %0, %%cr4" :: "r"(cr4));

	arch_syscall_init();
}

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
	ctx->cs     = GDT_USER_CODE;
	ctx->rflags = ARCH_SPSR_USER_TASK;
	ctx->rsp    = ustack_top;
	ctx->ss     = GDT_USER_DATA;
}