#define PTRACE_TRACEME    0
#define PTRACE_PEEKDATA   2
#define PTRACE_POKEDATA   5
#define PTRACE_CONT       7
#define PTRACE_SINGLESTEP 9
#define PTRACE_GETREGS   12
#define PTRACE_SETREGS   13
#define PTRACE_ATTACH    16
#define PTRACE_DETACH    17
#define PTRACE_SYSCALL   24

#if defined(__x86_64__)
typedef struct {
	uint64_t r15, r14, r13, r12, rbp, rbx;
	uint64_t r11, r10, r9, r8;
	uint64_t rax, rcx, rdx, rsi, rdi, orig_rax;
	uint64_t rip, cs, eflags, rsp, ss;
	uint64_t fs_base, gs_base, ds, es, fs, gs;
} ptrace_regs_t;
#else
typedef struct {
	uint64_t regs[31];
	uint64_t sp, pc, pstate;
} ptrace_regs_t;
#endif

static void ptrace_do_stop(process_t *p)
{
	process_t *tracer = proc_get(p->tracer);
	p->ptrace_in_syscall = 1;
	if (tracer) {
		task_wake(tracer);
		proc_put(tracer);
	}
	task_sleep_interruptible();   /* resumed by the tracer's task_wake (PTRACE_CONT) or a signal */
	p->ptrace_in_syscall = 0;
}

/* A debug trap (single-step #DB / breakpoint #BP) hit the current traced
 * process: report a SIGTRAP stop to the tracer, sleep until continued. Returns 1
 * if traced (handled — don't kill). Called from the arch exception path. */
int ptrace_trap(void)
{
	process_t *p = task_current();
	if (!p->tracer) return 0;
	p->ptrace_stop          = 1;
	p->ptrace_stop_reported = 0;
	ptrace_do_stop(p);
	p->ptrace_stop          = 0;
	p->ptrace_stop_reported = 0;
	return 1;
}

/* Applied at every return to user mode (from signal_dispatch and exc_dispatch):
 * the single-step state is a per-process flag, not a saved-frame bit, so it
 * survives the scheduler's context save/restore across the ptrace stop. TF is
 * (re)set on whatever frame is about to iret to user — Linux's TIF_SINGLESTEP
 * model. aarch64 software-step is a separate mechanism (TODO). */
void ptrace_apply_singlestep(arch_context_t *c)
{
#if defined(__x86_64__)
	process_t *p = task_current();
	if (!p || (c->cs & 3) != 3) return;   /* only on user returns */
	if (p->single_step) c->rflags |= (1UL << 8);   /* RFLAGS.TF */
	else                c->rflags &= ~(1UL << 8);
#else
	(void)c;
#endif
}

static void ptrace_stop_check(arch_context_t *ctx, int is_exit)
{
	process_t *p = task_current();
	if (!p->tracer || p->ptrace_opts != 1)
		return;
	p->ptrace_stop          = is_exit ? 2 : 1;
	p->ptrace_stop_reported = 0;
	ptrace_do_stop(p);
	p->ptrace_stop          = 0;
	p->ptrace_stop_reported = 0;
}

static long ptrace_req(process_t *p, process_t *tracee, long request, uint64_t addr, uint64_t data)
{
	switch (request) {
	case PTRACE_ATTACH:
		if (tracee->tracer != 0)
			return -1; /* EPERM */
		tracee->tracer      = p->pid;
		tracee->ptrace_opts = 0;
		task_signal(tracee, 19); /* SIGSTOP */
		return 0;

	case PTRACE_DETACH:
		if (tracee->tracer != (pid_t)p->pid)
			return -1;
		tracee->tracer              = 0;
		tracee->ptrace_opts         = 0;
		tracee->ptrace_stop         = 0;
		tracee->ptrace_stop_reported = 0;
		if (tracee->ptrace_in_syscall) {
			tracee->ptrace_in_syscall = 0;
			task_wake(tracee);
		}
		return 0;

	case PTRACE_CONT:
	case PTRACE_SYSCALL:
	case PTRACE_SINGLESTEP:
		if (tracee->tracer != (pid_t)p->pid)
			return -1;
		tracee->ptrace_opts          = (request == PTRACE_SYSCALL) ? 1 : 0;
		tracee->single_step          = (request == PTRACE_SINGLESTEP) ? 1 : 0;
		tracee->ptrace_stop          = 0;
		tracee->ptrace_stop_reported = 0;
		if (tracee->ptrace_in_syscall) {
			tracee->ptrace_in_syscall = 0;
			task_wake(tracee);
		}
		return 0;

	case PTRACE_GETREGS: {
		if (tracee->tracer != (pid_t)p->pid) return -1;   /* only the tracer may peek */
		arch_context_t *c = tracee->uctx ? tracee->uctx : tracee->ctx;
		if (!c)
			return -5; /* EIO */
		ptrace_regs_t ur;
		__builtin_memset(&ur, 0, sizeof(ur));
#if defined(__x86_64__)
		ur.r15 = c->r15; ur.r14 = c->r14; ur.r13 = c->r13; ur.r12 = c->r12;
		ur.rbp = c->rbp; ur.rbx = c->rbx;
		ur.r11 = c->r11; ur.r10 = c->r10; ur.r9  = c->r9;  ur.r8  = c->r8;
		ur.rax = c->rax; ur.rcx = c->rcx; ur.rdx = c->rdx;
		ur.rsi = c->rsi; ur.rdi = c->rdi; ur.orig_rax = c->rax;
		ur.rip = c->rip; ur.cs  = c->cs;  ur.eflags  = c->rflags;
		ur.rsp = c->rsp; ur.ss  = c->ss;
		ur.fs_base = tracee->tls_base;
#else
		for (int i = 0; i < 31; i++) ur.regs[i] = c->regs[i];
		ur.sp     = c->sp;
		ur.pc     = c->elr;
		ur.pstate = c->spsr;
#endif
		return copy_to_user((void *)data, &ur, sizeof(ur));
	}

	case PTRACE_SETREGS: {
		if (tracee->tracer != (pid_t)p->pid) return -1;
		arch_context_t *c = tracee->uctx ? tracee->uctx : tracee->ctx;
		if (!c)
			return -5;
		ptrace_regs_t ur;
		if (copy_from_user(&ur, (const void *)data, sizeof(ur)) != 0)
			return -14;
#if defined(__x86_64__)
		c->r15 = ur.r15; c->r14 = ur.r14; c->r13 = ur.r13; c->r12 = ur.r12;
		c->rbp = ur.rbp; c->rbx = ur.rbx;
		c->r11 = ur.r11; c->r10 = ur.r10; c->r9  = ur.r9;  c->r8  = ur.r8;
		c->rax = ur.rax; c->rcx = ur.rcx; c->rdx = ur.rdx;
		c->rsi = ur.rsi; c->rdi = ur.rdi;
		c->rip = ur.rip;
		/* a tracee runs at ring 3 — sanitize like rt_sigreturn: keep the user
		 * arithmetic flags, force IF=1 / IOPL=0 so SETREGS can't disable IRQs or
		 * grant I/O privilege in the tracee. */
		c->rflags = (ur.eflags & 0x00000CD5UL) | 0x202UL;
		c->rsp = ur.rsp;
#else
		for (int i = 0; i < 31; i++) c->regs[i] = ur.regs[i];
		c->sp   = ur.sp;
		c->elr  = ur.pc;
		/* keep only NZCV; force EL0t + IRQs-enabled so SETREGS can't make the
		 * tracee eret into EL1 (local root). Same sanitize as rt_sigreturn. */
		c->spsr = ur.pstate & 0xF0000000UL;
#endif
		return 0;
	}

	case PTRACE_PEEKDATA: {
		extern uint64_t arch_vm_get_paddr(uint64_t *root, uint64_t vaddr);
		if (tracee->tracer != (pid_t)p->pid) return -1;
		/* addr is fully user-controlled and dereferenced at EL1 after switching to
		 * the tracee's space. The kernel half is mapped in every space, so without
		 * a bound an addr >= VIRT_OFFSET would read kernel memory. Require a mapped
		 * user address in the tracee. */
		if (!access_ok((void *)addr, sizeof(uint64_t)) ||
		    !arch_vm_get_paddr(tracee->vm->root, addr & ~0xFFFUL))
			return -14;
		uint64_t val = 0;
		arch_vm_switch((uint64_t)tracee->vm->root, tracee->vm->asid);
		val = *(volatile uint64_t *)addr;
		arch_vm_switch((uint64_t)p->vm->root, p->vm->asid);
		(void)data;
		return (long)val;
	}

	case PTRACE_POKEDATA: {
		extern uint64_t arch_vm_get_paddr(uint64_t *root, uint64_t vaddr);
		if (tracee->tracer != (pid_t)p->pid) return -1;
		if (!access_ok((void *)addr, sizeof(uint64_t)) ||
		    !arch_vm_get_paddr(tracee->vm->root, addr & ~0xFFFUL))
			return -14;
		arch_vm_switch((uint64_t)tracee->vm->root, tracee->vm->asid);
		*(volatile uint64_t *)addr = (uint64_t)data;
		arch_vm_switch((uint64_t)p->vm->root, p->vm->asid);
		return 0;
	}

	default:
		return -38; /* ENOSYS */
	}
}

/* tracee is looked up by user-supplied pid; pin it (proc_get) across the whole
 * request so a concurrent exit+reap can't free it mid-operation. */
static long sys_ptrace(long request, pid_t pid, uint64_t addr, uint64_t data)
{
	process_t *p = task_current();

	if (request == PTRACE_TRACEME) {
		p->tracer    = p->ppid;
		p->ptrace_opts = 0;
		return 0;
	}

	process_t *tracee = proc_get(pid);
	if (!tracee)
		return -3; /* ESRCH */

	long ret = ptrace_req(p, tracee, request, addr, data);
	proc_put(tracee);
	return ret;
}
