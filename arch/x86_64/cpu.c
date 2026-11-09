/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
#include <kernel/hal.h>
#include <kernel/kprintf.h>
#include <kernel/lib/string.h>
#include <asm/gdt.h>
#include <kernel/proc.h>
#include <asm/cpu.h>

uint8_t avx_on;                     /* read by the entry stubs */

#define MSR_FS_BASE 0xC0000100U

void arch_save_tls(process_t *p)
{
	uint32_t lo, hi;
	__asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(MSR_FS_BASE));
	p->tls_base = ((uint64_t)hi << 32) | lo;
}

void arch_restore_tls(process_t *p)
{
	uint32_t lo = (uint32_t)p->tls_base;
	uint32_t hi = (uint32_t)(p->tls_base >> 32);
	__asm__ volatile("wrmsr" :: "c"(MSR_FS_BASE), "a"(lo), "d"(hi));
	/* in the kernel the user's GS base is the one swapgs has put aside */
	__asm__ volatile("wrmsr" :: "c"(0xC0000102U), "a"((uint32_t)p->user_gs_base),
	                 "d"((uint32_t)(p->user_gs_base >> 32)));
}


static cpu_context_t early_cpu_ctx;
static uint8_t early_irq_stack[4096] __attribute__((aligned(16)));

void arch_set_user_kstack(uint64_t kstack_top)
{
	tss_set_rsp0(kstack_top);
	/* Update the fast-access pointer in per-CPU area */
	arch_get_cpu_ctx()->kstack_top = (uint8_t *)kstack_top;
}

void arch_halt(void)  { __asm__ volatile("hlt"); }

/* MWAIT reaches the states that shut a core's clocks and caches; HLT stops at C1. The
 * hint is taken only if the local timer survives there (ARAT) and CPUID lists the state. */
static uint32_t mwait_hint;
static int mwait_ok;

void arch_idle_init(void)
{
	uint32_t a, b, c, d;
	__asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0));
	if (a < 6) return;
	__asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1));
	if (!(c & (1u << 3))) return;              /* MONITOR/MWAIT */
	__asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(6), "c"(0));
	if (!(a & (1u << 2))) return;              /* ARAT */
	__asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(5), "c"(0));
	if (!(c & 1)) return;
	for (int n = 3; n >= 1; n--)               /* C6, C3, C1: hint (n-1) << 4 */
		if ((d >> (n * 4)) & 0xF) {
			mwait_hint = (uint32_t)(n - 1) << 4;
			mwait_ok = 1;
			kprintf(KERN_INFO "cpu: idle by mwait, hint 0x%x\n", mwait_hint);
			return;
		}
}

/* /sys/kernel/cstate: how deep idle goes, 0 for HLT only, n for C-state n of MWAIT */
int arch_idle_get(void) { return mwait_ok ? (int)(mwait_hint >> 4) + 1 : 0; }

int arch_idle_set(int n)
{
	if (n < 0 || n > 8) return -22;
	mwait_ok = n > 0;
	mwait_hint = n > 0 ? (uint32_t)(n - 1) << 4 : 0;
	return 0;
}

void arch_idle_deep(volatile int *flag, int deep)
{
	if (!deep || !mwait_ok) {
		arch_idle_halt(flag);
		return;
	}
	__asm__ volatile("cli");
	__asm__ volatile("monitor" :: "a"(flag), "c"(0), "d"(0));
	if (*flag) { __asm__ volatile("sti"); return; }
	/* as with sti;hlt, nothing can land between the two */
	__asm__ volatile("sti; mwait" :: "a"(mwait_hint), "c"(0));
}

void arch_idle_halt(volatile int *flag)
{
	__asm__ volatile("cli");
	if (*flag) { __asm__ volatile("sti"); return; }
	/* sti enables IRQs only after the *next* instruction, so sti;hlt is an
	 * atomic pair: a resched IPI cannot slip in between, and the hlt wakes on it. */
	__asm__ volatile("sti; hlt");
}

void arch_yield(void) { __asm__ volatile("int $0x40"); }

cpu_context_t *arch_get_cpu_ctx(void)
{
	cpu_context_t *ctx;
	__asm__ volatile("movq %%gs:0, %0" : "=r"(ctx));
	return ctx;
}

void arch_syscall_init(void)
{
	/* EFER.SCE — enable syscall/sysret. EFER.NXE is set earlier in the boot/
	 * trampoline asm, before any NX-tagged PTE is walked. */
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
		"movq %0, %%rax\n"
		"movq %%rax, %%rdx\n"
		"shrq $32, %%rdx\n"
		"wrmsr\n"
		:: "r"(star) : "rax", "ecx", "rdx"
	);

	/* LSTAR */
	uint64_t lstar = (uint64_t)syscall_entry;
	__asm__ volatile(
		"movl $0xC0000082, %%ecx\n"
		"movq %0, %%rax\n"
		"movq %%rax, %%rdx\n"
		"shrq $32, %%rdx\n"
		"wrmsr\n"
		:: "r"(lstar) : "rax", "ecx", "rdx"
	);

	/* SFMASK: IF, and every flag user code can hand the kernel at syscall entry.
	 * TF would trap on the first kernel instruction, still on the user stack with
	 * the user's GS; DF reverses the kernel's string copies; NT makes iretq fault. */
	__asm__ volatile(
		"movl $0xC0000084, %%ecx\n"
		"movl $0x47700, %%eax\n"
		"xorl %%edx, %%edx\n"
		"wrmsr\n"
		::: "eax", "ecx", "edx"
	);
}

/* PAT entry 1 (PWT alone, unused otherwise) becomes write-combining. Every CPU
 * must agree. A framebuffer written through it is many times faster than uncached. */
void arch_pat_init(void)
{
	uint32_t lo, hi;
	__asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0x277U));
	lo = (lo & 0xFFFF00FFU) | (0x01U << 8);
	__asm__ volatile("wrmsr" :: "c"(0x277U), "a"(lo), "d"(hi));
}

/* Let the CPU choose its own frequency (HWP): left alone it sits at the base
 * clock with no turbo. Measured on Casper: one thread 30% slower than Linux, four
 * threads 64%. Asks for the full range, leaning to performance as Linux does. */
void arch_hwp_init(void)
{
	uint32_t a;
	__asm__ volatile("cpuid" : "=a"(a) : "a"(6) : "ebx", "ecx", "edx");
	if (!(a & (1U << 7)))
		return;
	__asm__ volatile("wrmsr" :: "c"(0x770U), "a"(1U), "d"(0U));
	uint32_t lo, hi;
	__asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0x771U));
	uint32_t highest = lo & 0xFF, lowest = lo >> 24;
	uint32_t req = lowest | highest << 8 | 0x80U << 24;
	__asm__ volatile("wrmsr" :: "c"(0x774U), "a"(req), "d"(0U));
}

void arch_cpu_init(void)
{
	/* Enable per-CPU area EARLY */
	early_cpu_ctx.self       = &early_cpu_ctx;
	early_cpu_ctx.current    = NULL;
	early_cpu_ctx.kstack_top = early_irq_stack + 4096;
	early_cpu_ctx.irq_stack  = early_irq_stack + 4096;
	early_cpu_ctx.cpu_id     = 0;
	early_cpu_ctx.magic      = 0xCAFEBABE;

	uint64_t gs_base = (uint64_t)&early_cpu_ctx;
	/* Set BOTH GS_BASE and KERNEL_GS_BASE to kernel context.
	 * When user syscalls, swapgs will flip them. */
	__asm__ volatile(
		"movl $0xC0000101, %%ecx\n" /* MSR_GS_BASE */
		"movq %0, %%rax\n"
		"movq %%rax, %%rdx\n"
		"shrq $32, %%rdx\n"
		"wrmsr\n"
		"movl $0xC0000102, %%ecx\n" /* MSR_KERNEL_GS_BASE */
		"wrmsr\n"
		:: "r"(gs_base) : "rax", "ecx", "rdx"
	);

	/* enable SSE and FXSAVE; CR0.WP so ring-0 stores honour the read-only PTE bit.
	 * Without WP, a kernel write to a COW-write-protected user page (e.g. pipe_read
	 * filling a user buffer that straddles a fresh COW page) writes through to the
	 * still-shared physical page instead of faulting, corrupting both mappings. */
	uint64_t cr0, cr4_val;
	__asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
	cr0 &= ~(1ULL << 2);
	cr0 |= (1ULL << 1) | (1ULL << 16);
	__asm__ volatile("mov %0, %%cr0" :: "r"(cr0));

	__asm__ volatile("mov %%cr4, %0" : "=r"(cr4_val));
	cr4_val |= (1ULL << 9);
	cr4_val |= (1ULL << 10);

	/* SMEP (CR4.20), gated on CPUID.(EAX=7,ECX=0):EBX bit7. The kernel can no
	 * longer fetch instructions from a user page — a CPU backstop on top of the
	 * W^X/NX page attributes. SMAP (CR4.21) is intentionally left off: it would
	 * fault every kernel read/write of a user buffer, and the fops read/write
	 * paths still touch user buffers directly (some via copy_*_user, some via raw
	 * memcpy), so enabling it needs every fop funnelled through the uaccess
	 * helpers first. */
	uint32_t ebx7;
	__asm__ volatile("movl $7, %%eax\n\txorl %%ecx, %%ecx\n\tcpuid\n"
		: "=b"(ebx7) :: "eax", "ecx", "edx");
	if (ebx7 & (1U << 7)) cr4_val |= (1ULL << 20);   /* SMEP */
	/* AVX. Software that reads CPUID without asking the OS (libopus does) runs
	 * ymm code regardless, and video and crypto want it anyway. The upper halves
	 * are kept beside the FXSAVE image; AVX-512 stays off, its state is not saved. */
	uint32_t ecx1;
	__asm__ volatile("movl $1, %%eax\n\tcpuid\n" : "=c"(ecx1) :: "eax", "ebx", "edx");
	int avx = (ecx1 & (1U << 26)) && (ecx1 & (1U << 28));
	if (avx) cr4_val |= (1ULL << 18);                /* OSXSAVE */
	__asm__ volatile("mov %0, %%cr4" :: "r"(cr4_val));
	if (avx) {
		__asm__ volatile("xsetbv" :: "a"(7), "d"(0), "c"(0));   /* x87, SSE, AVX */
		avx_on = 1;
	}

	arch_pat_init();
	arch_hwp_init();
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
	/* Architectural FPU/SSE reset state for a fresh process: the first
	 * return-to-user fxrstors this image, so a zeroed area would load
	 * FCW=0 / MXCSR=0 instead of the standard 0x037f / 0x1f80. */
	*(uint16_t *)&ctx->fxsave[0]  = 0x037f;   /* FCW */
	*(uint32_t *)&ctx->fxsave[24] = 0x1f80;   /* MXCSR */
}
