#include <kernel/hal.h>
#include <kernel/lib/string.h>
#include <asm/gdt.h>

extern void syscall_entry(void);

void arch_set_user_kstack(uint64_t kstack_top)
{
	tss_set_rsp0(kstack_top);
}

void arch_halt(void)  { __asm__ volatile("hlt"); }
void arch_yield(void) { __asm__ volatile("int $0x40"); }

static void arch_syscall_init(void)
{
	/* EFER.SCE — enable syscall/sysret */
	__asm__ volatile(
		"movl $0xC0000080, %%ecx\n"
		"rdmsr\n"
		"orl  $1, %%eax\n"
		"wrmsr\n"
		::: "eax", "ecx", "edx"
	);

	/* STAR[47:32]=kernel CS (0x08), STAR[63:48]=user CS base (0x18)
	 * sysret: CS = STAR[63:48]|3 = 0x1B, SS = STAR[63:48]+8|3 = 0x23 */
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

	/* LSTAR — syscall entry address */
	uint64_t lstar = (uint64_t)syscall_entry;
	__asm__ volatile(
		"movl $0xC0000082, %%ecx\n"
		"movq %0, %%rax\n"
		"movq %%rax, %%rdx\n"
		"shrq $32, %%rdx\n"
		"wrmsr\n"
		:: "r"(lstar) : "rax", "ecx", "edx"
	);

	/* SFMASK — clear IF on syscall entry */
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
	cr0 &= ~(1ULL << 2); /* clear EM (Emulation) */
	cr0 |= (1ULL << 1);  /* set MP (Monitor Coprocessor) */
	__asm__ volatile("mov %0, %%cr0" :: "r"(cr0));

	__asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
	cr4 |= (1ULL << 9);  /* set OSFXSR (FXSAVE/FXRSTOR support) */
	cr4 |= (1ULL << 10); /* set OSXMMEXCPT (SIMD exceptions) */
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
	ctx->cs     = GDT_USER_CODE;   /* ring3 code: 0x1B */
	ctx->rflags = ARCH_SPSR_USER_TASK;
	ctx->rsp    = ustack_top;
	ctx->ss     = GDT_USER_DATA;   /* ring3 data: 0x23 */
}
