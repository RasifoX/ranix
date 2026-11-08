#include <kernel/board.h>
#include <kernel/ksyms.h>
#include <kernel/hal.h>
#include <asm/context.h>
#include <asm/gdt.h>
#include <asm/idt.h>
#include <kernel/kprintf.h>
#include <kernel/mm/vm.h>
#include <kernel/proc.h>
#include <kernel/extable.h>

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
	extern int  ptrace_trap(int si_code);
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
			extern int slowsys_on; extern uint64_t slowsys_ns(void);
			uint64_t ft0 = slowsys_on > 0 ? slowsys_ns() : 0;
			{	/* the kernel touched user memory with a spinlock held: the fault
				 * may sleep or shoot TLBs, which that lock forbids. Name the place. */
				extern int spin_held_count(void);
				extern void spin_dump_held(void);
				static int told;
				if (!(ctx->cs & 3) && spin_held_count() > 0 && told < 5) {
					told++;
					uint64_t off = 0;
					const char *sym = ksym_lookup(ctx->rip, &off);
					kprintf(KERN_ERR "fault: user page %p touched under a spinlock at %s+0x%lx\n",
					        (void *)cr2, sym ? sym : "?", (unsigned long)off);
					spin_dump_held();
				}
			}
			int fr = vm_handle_fault(cr2, is_write);
			if (ft0) { process_t *fc = task_current(); if (fc) { fc->ss_faults++; fc->ss_fault_ns += slowsys_ns() - ft0; } }
			if (fr == 0) {
				ptrace_apply_singlestep(ctx);
				return;
			}
		}
	}

	/* Debug traps: single-step (#DB) or breakpoint (#BP, int3). A traced ring-3
	 * process reports a SIGTRAP stop; a stray #DB just resumes (don't panic).
	 * TF is re-armed by ptrace_apply_singlestep on the way back to user. */
	if (ctx->vector == 1) {
		if ((ctx->cs & 3) == 3) ptrace_trap(2 /* TRAP_TRACE */);
		ptrace_apply_singlestep(ctx);
		return;
	}
	if (ctx->vector == 3 && (ctx->cs & 3) == 3) {
		if (ptrace_trap(0x80 /* SI_KERNEL: int3 */)) { ptrace_apply_singlestep(ctx); return; }
	}

	/* Ring-3 fault: the program's signal, which is the end of it without a handler */
	if ((ctx->cs & 3) == 3) {
		process_t *p = task_current();
		int sig = SIGSEGV, code = 0x80 /* SI_KERNEL */;
		uint64_t addr = 0;
		switch (ctx->vector) {
		case 0:  sig = SIGFPE;  code = 1 /* FPE_INTDIV */;  addr = ctx->rip; break;
		case 3:  sig = SIGTRAP; code = 1 /* TRAP_BRKPT */;  addr = ctx->rip; break;
		case 6:  sig = SIGILL;  code = 2 /* ILL_ILLOPN */;  addr = ctx->rip; break;
		case 14: addr = read_cr2();
		         code = (ctx->error_code & 1) ? 2 /* SEGV_ACCERR */ : 1 /* SEGV_MAPERR */; break;
		case 16: case 19:
		         sig = SIGFPE;  code = 7 /* FPE_FLTINV */;  addr = ctx->rip; break;
		case 17: sig = SIGBUS;  code = 1 /* BUS_ADRALN */;  addr = ctx->rip; break;
		default: break;
		}
		if (task_fault_fatal(sig)) {
			kprintf(KERN_WARN "fault: segfault pid=%d (%s) rip=0x%lx vec=%d err=0x%lx\n",
			        (int)p->pid, p->name, ctx->rip, (int)ctx->vector, ctx->error_code);
			if (ctx->vector == 14)
				kprintf(KERN_WARN "fault: cr2=0x%lx\n", addr);
			kprintf(KERN_WARN "fault: rax=0x%lx rbx=0x%lx rbp=0x%lx rsp=0x%lx\n",
			        ctx->rax, ctx->rbx, ctx->rbp, ctx->rsp);
			kprintf(KERN_WARN "fault: rdi=0x%lx rsi=0x%lx rdx=0x%lx rcx=0x%lx\n",
			        ctx->rdi, ctx->rsi, ctx->rdx, ctx->rcx);
		}
		task_force_fault(ctx, sig, code, addr, ctx->error_code, ctx->vector);
		return;
	}

	unsigned long fixup = search_extable(ctx->rip);
	if (fixup) {
		ctx->rip = fixup;
		return;
	}

	panic_report(exc_name(ctx->vector), ctx);
}

/* ---- sampling profiler: where the CPUs are, at every timer interrupt ----
 * /dev/prof: write 1 to start (the buffer empties), 0 to stop, read the samples:
 * 24 bytes each: rip, caller, pid, cpu. For finding what eats a live machine. */
#include <kernel/fs/devfs.h>
#include <kernel/proc.h>
#include <kernel/vfs.h>
#define PROF_N (1U << 16)
typedef struct { uint64_t rip, caller; uint32_t pid; uint32_t cpu; } prof_t;
static prof_t prof_buf[PROF_N];
static volatile uint32_t prof_n;
static volatile int prof_on;

/* Soft lockup: a task that has held a CPU in the kernel for seconds without once
 * switching starves everything queued behind it (the kernel is not preemptible).
 * Say where it is, from the interrupted frame, every 5 s while it lasts. */
static void softlockup_check(arch_context_t *ctx)
{
	static struct { process_t *p; uint64_t sw, since, told; } st[64];
	extern volatile uint64_t kernel_ticks;
	cpu_context_t *c = arch_get_cpu_ctx();
	process_t *p = task_current();
	int i = (int)c->cpu_id & 63;
	if (!p || !p->pid || (ctx->cs & 3)) { st[i].p = NULL; return; }
	if (st[i].p != p || st[i].sw != p->hung_switches) {
		st[i].p = p; st[i].sw = p->hung_switches; st[i].since = kernel_ticks; st[i].told = 0;
		return;
	}
	uint64_t held = kernel_ticks - st[i].since;
	if (held < 5 * TIMER_HZ || held - st[i].told < 5 * TIMER_HZ) return;
	st[i].told = held;
	uint64_t r1 = 0, r2 = 0, fp = ctx->rbp;
	if (fp >= 0xFFFF000000000000UL && !(fp & 7)) {
		r1 = ((uint64_t *)fp)[1];
		uint64_t f2 = ((uint64_t *)fp)[0];
		if (f2 > fp && f2 - fp < 16384) r2 = ((uint64_t *)f2)[1];
	}
	kprintf(KERN_ERR "softlockup: cpu%d pid %d (%s) in the kernel for %lu s at %p <- %p <- %p\n",
	        i, (int)p->pid, p->name, (unsigned long)(held / TIMER_HZ), (void *)ctx->rip, (void *)r1, (void *)r2);
}

void prof_sample(arch_context_t *ctx)
{
	softlockup_check(ctx);
	if (!prof_on) return;
	uint32_t i = __sync_fetch_and_add(&prof_n, 1);
	if (i >= PROF_N) { prof_on = 0; return; }
	process_t *p = task_current();
	/* In the kernel the top of stack is usually who called: the leaves that sample
	 * most (irq_restore) say nothing by themselves. */
	uint64_t caller = 0;
	if (!(ctx->cs & 3)) {
		/* the frame chain names who called even when the top of stack is data;
		 * only followed when it stays on this stack, asm may use rbp otherwise */
		caller = *(uint64_t *)ctx->rsp;
		if (ctx->rbp > ctx->rsp && ctx->rbp - ctx->rsp < 8192 && !(ctx->rbp & 7))
			caller = *(uint64_t *)(ctx->rbp + 8);
	}
	prof_buf[i] = (prof_t){ctx->rip, caller, p ? (uint32_t)p->pid : 0, (uint32_t)arch_get_cpu_ctx()->cpu_id};
}

static ssize_t prof_read(vfs_file_t *f, void *buf, size_t len)
{
	size_t have = (size_t)prof_n * sizeof(prof_t);
	if (have > sizeof prof_buf) have = sizeof prof_buf;
	if ((size_t)f->pos >= have) return 0;
	if (len > have - (size_t)f->pos) len = have - (size_t)f->pos;
	if (copy_fop_to_user(buf, (uint8_t *)prof_buf + f->pos, len)) return -14;
	f->pos += (off_t)len;
	return (ssize_t)len;
}

static ssize_t prof_write(vfs_file_t *f, const void *buf, size_t len)
{
	(void)f;
	char c = 0;
	if (len && copy_fop_from_user(&c, buf, 1)) return -14;
	if (c == '1') { prof_n = 0; prof_on = 1; } else prof_on = 0;
	return (ssize_t)len;
}

static vfs_fops_t prof_fops = { .read = prof_read, .write = prof_write };

void prof_init(void)
{
	devfs_register_device("prof", S_IFCHR | 0600, 0x0F05, &prof_fops);
}
