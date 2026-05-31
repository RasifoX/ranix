#define PTRACE_TRACEME    0
#define PTRACE_PEEKDATA   2
#define PTRACE_POKEDATA   5
#define PTRACE_CONT       7
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
	process_t *tracer = proc_find(p->tracer);
	p->ptrace_in_syscall = 1;
	if (tracer)
		task_wake(tracer);
	task_sleep_interruptible();   /* resumed by the tracer's task_wake (PTRACE_CONT) or a signal */
	p->ptrace_in_syscall = 0;
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

static long sys_ptrace(long request, pid_t pid, uint64_t addr, uint64_t data)
{
	process_t *p = task_current();

	if (request == PTRACE_TRACEME) {
		p->tracer    = p->ppid;
		p->ptrace_opts = 0;
		return 0;
	}

	process_t *tracee = proc_find(pid);
	if (!tracee)
		return -3; /* ESRCH */

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
		if (tracee->tracer != (pid_t)p->pid)
			return -1;
		tracee->ptrace_opts          = (request == PTRACE_SYSCALL) ? 1 : 0;
		tracee->ptrace_stop          = 0;
		tracee->ptrace_stop_reported = 0;
		if (tracee->ptrace_in_syscall) {
			tracee->ptrace_in_syscall = 0;
			task_wake(tracee);
		}
		return 0;

	case PTRACE_GETREGS: {
		if (!tracee->ctx)
			return -5; /* EIO */
		ptrace_regs_t ur;
		__builtin_memset(&ur, 0, sizeof(ur));
		arch_context_t *c = tracee->ctx;
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
		if (!tracee->ctx)
			return -5;
		ptrace_regs_t ur;
		if (copy_from_user(&ur, (const void *)data, sizeof(ur)) != 0)
			return -14;
		arch_context_t *c = tracee->ctx;
#if defined(__x86_64__)
		c->r15 = ur.r15; c->r14 = ur.r14; c->r13 = ur.r13; c->r12 = ur.r12;
		c->rbp = ur.rbp; c->rbx = ur.rbx;
		c->r11 = ur.r11; c->r10 = ur.r10; c->r9  = ur.r9;  c->r8  = ur.r8;
		c->rax = ur.rax; c->rcx = ur.rcx; c->rdx = ur.rdx;
		c->rsi = ur.rsi; c->rdi = ur.rdi;
		c->rip = ur.rip; c->rflags = ur.eflags;
		c->rsp = ur.rsp;
#else
		for (int i = 0; i < 31; i++) c->regs[i] = ur.regs[i];
		c->sp   = ur.sp;
		c->elr  = ur.pc;
		c->spsr = ur.pstate;
#endif
		return 0;
	}

	case PTRACE_PEEKDATA: {
		uint64_t val = 0;
		arch_vm_switch((uint64_t)tracee->vm->root);
		val = *(volatile uint64_t *)addr;
		arch_vm_switch((uint64_t)p->vm->root);
		(void)data;
		return (long)val;
	}

	case PTRACE_POKEDATA: {
		arch_vm_switch((uint64_t)tracee->vm->root);
		*(volatile uint64_t *)addr = (uint64_t)data;
		arch_vm_switch((uint64_t)p->vm->root);
		return 0;
	}

	default:
		return -38; /* ENOSYS */
	}
}
