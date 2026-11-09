/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
#include <kernel/capabilities.h>
#include <kernel/errno.h>
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
#define PTRACE_PEEKTEXT   1
#define PTRACE_PEEKUSER   3
#define PTRACE_POKETEXT   4
#define PTRACE_POKEUSER   6
#define PTRACE_GETFPREGS 14
#define PTRACE_SETFPREGS 15
#define PTRACE_SETOPTIONS  0x4200
#define PTRACE_GETEVENTMSG 0x4201
#define PTRACE_GETSIGINFO  0x4202
#define PTRACE_SETSIGINFO  0x4203
#define PTRACE_GETREGSET   0x4204
#define PTRACE_SETREGSET   0x4205
#define PTRACE_O_TRACESYSGOOD 1
#define PTRACE_O_TRACEFORK    2
#define PTRACE_O_TRACEVFORK   4
#define PTRACE_O_TRACECLONE   8
#define PTRACE_O_TRACEEXEC    0x10
#define PTRACE_EVENT_FORK  1
#define PTRACE_EVENT_VFORK 2
#define PTRACE_EVENT_CLONE 3
#define PTRACE_EVENT_EXEC  4
#define NT_PRSTATUS 1
#define NT_FPREGSET 2
#define SIGTRAP_  5

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
	proc_wake_tracer(p);
	if (tracer) {
		task_wake(tracer);
		task_signal_group(tracer, SIGCHLD);   /* gdb sleeps in sigsuspend, not wait */
		proc_put(tracer);
	}
	/* resumed by the tracer's task_wake (PTRACE_CONT) or a signal */
	while (p->ptrace_in_syscall && p->tracer && !(p->sig_pending & (1UL << (SIGKILL - 1))))
		task_sleep_if(p->ptrace_in_syscall && p->tracer &&
		              !(p->sig_pending & (1UL << (SIGKILL - 1))));
	p->ptrace_in_syscall = 0;
}

/* A stopping signal (SIGSTOP/TSTP/TTIN/TTOU, including the one PTRACE_ATTACH
 * injects) hit a traced task: enter a ptrace group-stop. Reported to the tracer
 * regardless of waitpid WUNTRACED, and resumed by PTRACE_CONT/DETACH (which clear
 * ptrace_stop + wake) or a fatal signal. Called from signal_dispatch on the
 * user-return path. */
void ptrace_group_stop(int sig)
{
	process_t *p = task_current();
	p->job_stop_sig         = sig;
	p->ptrace_stop_sig      = sig;
	p->ptrace_in_syscall    = 1;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	p->ptrace_stop          = 1;
	p->ptrace_stop_reported = 0;
	arch_get_cpu_ctx()->wd_where = "ptrace:groupstop";

	process_t *tracer = proc_get(p->tracer);
	if (tracer) { task_wake(tracer); task_signal_group(tracer, SIGCHLD); proc_put(tracer); }
	/* Stay stopped until the tracer continues us (clears ptrace_stop) or detaches
	 * (clears tracer); a fatal SIGKILL also breaks out to terminate. A spurious
	 * wake (another posted signal) must not un-stop us — re-check and re-sleep. */
	while (p->ptrace_stop && p->tracer &&
	       !(p->sig_pending & (1UL << (SIGKILL - 1))))
		task_sleep_if(p->ptrace_stop && p->tracer &&
		              !(p->sig_pending & (1UL << (SIGKILL - 1))));
	p->ptrace_in_syscall    = 0;
	p->ptrace_stop          = 0;
	p->ptrace_stop_reported = 0;
	p->ptrace_stop_sig      = 0;
}

/* A debug trap (single-step #DB / breakpoint #BP) hit the current traced
 * process: report a SIGTRAP stop to the tracer, sleep until continued. Returns 1
 * if traced (handled — don't kill). Called from the arch exception path. */
int ptrace_trap(int si_code)
{
	process_t *p = task_current();
	if (!p->tracer) return 0;
	p->ptrace_si.si_signo   = SIGTRAP_;
	p->ptrace_si.si_code    = si_code;     /* how gdb tells a breakpoint from a step */
	p->ptrace_si.si_pid     = 0;
	p->ptrace_si.si_uid     = 0;
	p->ptrace_si.si_value   = 0;
	p->ptrace_in_syscall    = 1;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
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

static void ptrace_report(process_t *p, int status, int kind)
{
	/* in_syscall first: a tracer that sees the stop and resumes at once clears
	 * it, and the stop below then does not wait for a resume already given */
	p->ptrace_in_syscall    = 1;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	p->ptrace_stop_sig      = status;
	p->ptrace_stop          = kind;
	p->ptrace_stop_reported = 0;
	p->ptrace_si.si_signo   = status & 0x7f;
	p->ptrace_si.si_code    = 0;
	p->ptrace_si.si_pid     = 0;
	p->ptrace_si.si_uid     = 0;
	p->ptrace_si.si_value   = 0;
	ptrace_do_stop(p);
	p->ptrace_stop          = 0;
	p->ptrace_stop_reported = 0;
	p->ptrace_stop_sig      = 0;
}

static void ptrace_stop_check(arch_context_t *ctx, int is_exit)
{
	(void)ctx;
	process_t *p = task_current();
	if (!p->tracer) return;
	if (is_exit && p->ptrace_event) {            /* fork, clone or exec just finished */
		int ev = p->ptrace_event;
		p->ptrace_event = 0;
		ptrace_report(p, SIGTRAP_ | (ev << 8), 1);
	}
	if (p->ptrace_opts != 1) return;
	ptrace_report(p, SIGTRAP_ | ((p->ptrace_options & PTRACE_O_TRACESYSGOOD) ? 0x80 : 0), is_exit ? 2 : 1);
}

/* New task of a traced one: the tracer follows it if asked, and it starts stopped */
void ptrace_follow(process_t *parent, process_t *child, int kind, int event)
{
	if (!parent->tracer || !(parent->ptrace_options & kind)) return;
	child->tracer         = parent->tracer;
	child->ptrace_options = parent->ptrace_options;
	child->ptrace_opts    = parent->ptrace_opts;
	child->sig_pending   |= 1UL << (19 /* SIGSTOP */ - 1);
	parent->ptrace_event  = event;
	parent->ptrace_msg    = (uint64_t)child->pid;
}

void ptrace_follow_fork(process_t *parent, process_t *child)
{
	ptrace_follow(parent, child, PTRACE_O_TRACEFORK, PTRACE_EVENT_FORK);
}

void ptrace_follow_clone(process_t *parent, process_t *child)
{
	ptrace_follow(parent, child, PTRACE_O_TRACECLONE, PTRACE_EVENT_CLONE);
}

void ptrace_exec_done(process_t *p)
{
	if (!p->tracer) return;
	if (p->ptrace_options & PTRACE_O_TRACEEXEC) p->ptrace_event = PTRACE_EVENT_EXEC;
	else task_signal(p, SIGTRAP_);
}

/* The siginfo to deliver after a signal-delivery-stop (Linux ptrace_signal):
 * whatever the tracer left with PTRACE_SETSIGINFO; for a signal the tracer
 * replaced with another, one from the tracer (SI_USER, its pid and uid). */
static void ptrace_signal_info(process_t *p, int sig, sigqueue_t *si)
{
	if (!sig) return;
	sigqueue_t *next = si->next;
	*si = p->ptrace_si;
	si->next = next;
	if (sig == si->si_signo) return;
	process_t *t = proc_get(p->tracer);
	si->si_signo = sig;
	si->si_errno = 0;
	si->si_code  = 0;            /* SI_USER */
	si->si_pid   = pid_to_user(p->tracer);
	si->si_uid   = t ? t->uid : 0;
	si->si_value = 0;
	proc_put(t);
}

/* Every signal but SIGKILL stops a traced task before it takes effect. The tracer
 * sees it, then lets it through, replaces it or drops it. Returns what to deliver. */
int ptrace_signal_stop(arch_context_t *tf, int sig, sigqueue_t *si)
{
	process_t *p = task_current();
	if (!p->tracer || sig == SIGKILL) return sig;
	p->uctx = tf;
	p->ptrace_inject = sig;
	p->ptrace_stop_sig      = sig;
	p->ptrace_in_syscall    = 1;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	p->ptrace_stop          = 1;
	p->ptrace_stop_reported = 0;
	p->ptrace_si            = *si;
	ptrace_do_stop(p);
	p->ptrace_stop          = 0;
	p->ptrace_stop_reported = 0;
	p->ptrace_stop_sig      = 0;
	p->ctx = tf;                 /* sleeping saved a deeper frame there */
	ptrace_signal_info(p, p->ptrace_inject, si);
	return p->ptrace_inject;
}

/* Linux ptrace_may_access (ATTACH/REALCREDS mode). The caller may access the
 * target only if its euid matches the target's real, effective and saved uid
 * and its egid matches all three gids, and the target is dumpable — or it holds
 * CAP_SYS_PTRACE. Accessing oneself is always allowed. */
int ptrace_may_access(process_t *t)
{
	process_t *p = task_current();
	if (p == t) return 1;
	if (p->asess && p->asess != t->asess) return 0;
	if (capable_task(p, CAP_SYS_PTRACE)) return 1;
	if (p->euid != t->uid || p->euid != t->euid || p->euid != t->suid ||
	    p->egid != t->gid || p->egid != t->egid || p->egid != t->sgid)
		return 0;
	return t->dumpable != 0;
}

static int ptrace_get_regs(process_t *tracee, ptrace_regs_t *ur)
{
	arch_context_t *c = tracee->uctx ? tracee->uctx : tracee->ctx;
	if (!c) return -EIO;
	__builtin_memset(ur, 0, sizeof(*ur));
#if defined(__x86_64__)
	ur->r15 = c->r15; ur->r14 = c->r14; ur->r13 = c->r13; ur->r12 = c->r12;
	ur->rbp = c->rbp; ur->rbx = c->rbx;
	ur->r11 = c->r11; ur->r10 = c->r10; ur->r9  = c->r9;  ur->r8  = c->r8;
	ur->rax = c->rax; ur->rcx = c->rcx; ur->rdx = c->rdx;
	ur->rsi = c->rsi; ur->rdi = c->rdi;
	ur->orig_rax = (tracee->in_syscall || tracee->ptrace_stop == 2)
	             ? (uint64_t)(long)tracee->last_syscall : (uint64_t)-1;
	/* Linux's selectors, not ours: gdb tells 64-bit from 32-bit by the value of cs */
	ur->rip = c->rip; ur->cs  = 0x33;  ur->eflags  = c->rflags;
	ur->rsp = c->rsp; ur->ss  = 0x2b;
	ur->fs_base = tracee->tls_base;
#else
	for (int i = 0; i < 31; i++) ur->regs[i] = c->regs[i];
	ur->sp     = c->sp;
	ur->pc     = c->elr;
	ur->pstate = c->spsr;
#endif
	return 0;
}

static int ptrace_set_regs(process_t *tracee, const ptrace_regs_t *ur)
{
	arch_context_t *c = tracee->uctx ? tracee->uctx : tracee->ctx;
	if (!c) return -EIO;
#if defined(__x86_64__)
	c->r15 = ur->r15; c->r14 = ur->r14; c->r13 = ur->r13; c->r12 = ur->r12;
	c->rbp = ur->rbp; c->rbx = ur->rbx;
	c->r11 = ur->r11; c->r10 = ur->r10; c->r9  = ur->r9;  c->r8  = ur->r8;
	c->rax = ur->rax; c->rcx = ur->rcx; c->rdx = ur->rdx;
	c->rsi = ur->rsi; c->rdi = ur->rdi;
	c->rip = ur->rip;
	/* a tracee runs at ring 3 — sanitize like rt_sigreturn: keep the user
	 * arithmetic flags, force IF=1 / IOPL=0 so SETREGS can't disable IRQs or
	 * grant I/O privilege in the tracee. */
	c->rflags = (ur->eflags & 0x00000CD5UL) | 0x202UL;
	c->rsp = ur->rsp;
#else
	for (int i = 0; i < 31; i++) c->regs[i] = ur->regs[i];
	c->sp   = ur->sp;
	c->elr  = ur->pc;
	/* keep only NZCV; force EL0t + IRQs-enabled so SETREGS can't make the
	 * tracee eret into EL1 (local root). Same sanitize as rt_sigreturn. */
	c->spsr = ur->pstate & 0xF0000000UL;
#endif
	return 0;
}

static long ptrace_regset(process_t *tracee, uint64_t type, uint64_t uiov, int write)
{
	struct { void *base; uint64_t len; } iov;
	if (copy_from_user(&iov, (void *)uiov, sizeof(iov)) != 0) return -EFAULT;
	if (type == NT_PRSTATUS) {
		ptrace_regs_t ur;
		uint64_t n = iov.len < sizeof(ur) ? iov.len : sizeof(ur);
		int r = ptrace_get_regs(tracee, &ur);
		if (r) return r;
		if (write) {
			if (copy_from_user(&ur, iov.base, n) != 0) return -EFAULT;
			return ptrace_set_regs(tracee, &ur);
		}
		if (copy_to_user(iov.base, &ur, n) != 0) return -EFAULT;
		iov.len = n;
		return copy_to_user((void *)uiov, &iov, sizeof(iov)) ? -14 : 0;
	}
#if defined(__x86_64__)
	if (type == NT_FPREGSET) {
		arch_context_t *c = tracee->uctx ? tracee->uctx : tracee->ctx;
		uint64_t n = iov.len < sizeof(c->fxsave) ? iov.len : sizeof(c->fxsave);
		if (write) return copy_from_user(c->fxsave, iov.base, n) ? -14 : 0;
		if (copy_to_user(iov.base, c->fxsave, n) != 0) return -EFAULT;
		iov.len = n;
		return copy_to_user((void *)uiov, &iov, sizeof(iov)) ? -14 : 0;
	}
#endif
	return -EINVAL;
}

/* Options, event message and signal info of a stopped tracee. */
static long ptrace_info_req(process_t *p, process_t *tracee, long request, uint64_t addr, uint64_t data)
{
	switch (request) {
	case PTRACE_SETOPTIONS:
		if (tracee->tracer != (pid_t)p->pid) return -ESRCH;
		tracee->ptrace_options = (int)data;
		return 0;

	case PTRACE_GETEVENTMSG:
		if (tracee->tracer != (pid_t)p->pid) return -ESRCH;
		return copy_to_user((void *)data, &tracee->ptrace_msg, sizeof(uint64_t)) ? -14 : 0;

	case PTRACE_GETSIGINFO: {
		if (tracee->tracer != (pid_t)p->pid) return -ESRCH;
		int32_t info[32];
		__builtin_memset(info, 0, sizeof(info));
		info[0] = tracee->ptrace_si.si_signo;
		info[1] = tracee->ptrace_si.si_errno;
		info[2] = tracee->ptrace_si.si_code;
		info[4] = (int32_t)tracee->ptrace_si.si_pid;
		info[5] = (int32_t)tracee->ptrace_si.si_uid;
		__builtin_memcpy(&info[6], &tracee->ptrace_si.si_value, 8);
		return copy_to_user((void *)data, info, sizeof(info)) ? -14 : 0;
	}

	case PTRACE_SETSIGINFO: {
		/* what the tracee gets if continued with this signal (ptrace_signal_stop);
		 * a sigqueue_t holds the first 32 bytes, the rest are zero on delivery */
		if (tracee->tracer != (pid_t)p->pid) return -ESRCH;
		int32_t info[32];
		if (copy_from_user(info, (const void *)data, sizeof(info)) != 0) return -EFAULT;
		tracee->ptrace_si.si_signo = info[0];
		tracee->ptrace_si.si_errno = info[1];
		tracee->ptrace_si.si_code  = info[2];
		tracee->ptrace_si.si_pid   = (pid_t)info[4];
		tracee->ptrace_si.si_uid   = (uid_t)info[5];
		__builtin_memcpy(&tracee->ptrace_si.si_value, &info[6], 8);
		return 0;
	}

	default:
		return -ENOSYS;
	}
}

/* Read or write the tracee's registers (general, FP, by offset, by regset). */
static long ptrace_regs_req(process_t *p, process_t *tracee, long request, uint64_t addr, uint64_t data)
{
	switch (request) {
	case PTRACE_GETREGSET:
	case PTRACE_SETREGSET:
		if (tracee->tracer != (pid_t)p->pid) return -ESRCH;
		return ptrace_regset(tracee, addr, data, request == PTRACE_SETREGSET);

#if defined(__x86_64__)
	case PTRACE_GETFPREGS:
	case PTRACE_SETFPREGS: {
		if (tracee->tracer != (pid_t)p->pid) return -ESRCH;
		arch_context_t *c = tracee->uctx ? tracee->uctx : tracee->ctx;
		if (!c) return -EIO;
		if (request == PTRACE_GETFPREGS)
			return copy_to_user((void *)data, c->fxsave, sizeof(c->fxsave)) ? -14 : 0;
		if (copy_from_user(c->fxsave, (void *)data, sizeof(c->fxsave)) != 0) return -EFAULT;
		*(uint32_t *)&c->fxsave[24] &= 0xffff;   /* reserved MXCSR bits #GP in fxrstor */
		return 0;
	}

	case PTRACE_PEEKUSER: {
		if (tracee->tracer != (pid_t)p->pid) return -ESRCH;
		ptrace_regs_t ur;
		uint64_t val = 0;
		if (addr & 7) return -EIO;
		if (addr < sizeof(ur)) {
			int r = ptrace_get_regs(tracee, &ur);
			if (r) return r;
			val = ((uint64_t *)&ur)[addr / 8];
		}                            /* past the registers: debug registers, unused */
		return copy_to_user((void *)data, &val, sizeof(val)) ? -14 : 0;
	}

	case PTRACE_POKEUSER: {
		if (tracee->tracer != (pid_t)p->pid) return -ESRCH;
		ptrace_regs_t ur;
		if (addr & 7) return -EIO;
		if (addr >= sizeof(ur)) return 0;
		int r = ptrace_get_regs(tracee, &ur);
		if (r) return r;
		((uint64_t *)&ur)[addr / 8] = data;
		return ptrace_set_regs(tracee, &ur);
	}
#endif

	case PTRACE_GETREGS: {
		if (tracee->tracer != (pid_t)p->pid) return -ESRCH;   /* only the tracer may peek */
		ptrace_regs_t ur;
		int r = ptrace_get_regs(tracee, &ur);
		if (r) return r;
		return copy_to_user((void *)data, &ur, sizeof(ur)) ? -14 : 0;
	}

	case PTRACE_SETREGS: {
		if (tracee->tracer != (pid_t)p->pid) return -ESRCH;
		ptrace_regs_t ur;
		if (copy_from_user(&ur, (const void *)data, sizeof(ur)) != 0)
			return -EFAULT;
		return ptrace_set_regs(tracee, &ur);
	}

	default:
		return -ENOSYS;
	}
}

/* Attach, detach, and resume (continue, syscall stop, single step). */
static long ptrace_run_req(process_t *p, process_t *tracee, long request, uint64_t addr, uint64_t data)
{
	switch (request) {
	case PTRACE_ATTACH:
		if (tracee->tgid == p->tgid) return -EPERM;   /* no thread can trace its own group */
		if (!ptrace_may_access(tracee))
			return -EPERM;
		if (tracee->tracer != 0)
			return -EPERM;
		tracee->tracer      = p->pid;
		tracee->ptrace_opts = 0;
		tracee->ptrace_options = 0;
		task_signal(tracee, 19); /* SIGSTOP */
		return 0;

	case PTRACE_DETACH:
		if (tracee->tracer != (pid_t)p->pid)
			return -ESRCH;
		if (data > NSIG) return -EIO;   /* it indexes the signal actions on delivery */
		tracee->tracer              = 0;
		tracee->ptrace_opts         = 0;
		tracee->ptrace_options      = 0;
		tracee->ptrace_inject       = (int)data;
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
			return -ESRCH;
		if (data > NSIG) return -EIO;   /* it indexes the signal actions on delivery */
		tracee->ptrace_opts          = (request == PTRACE_SYSCALL) ? 1 : 0;
		tracee->single_step          = (request == PTRACE_SINGLESTEP) ? 1 : 0;
		tracee->ptrace_inject        = (int)data;
		tracee->ptrace_stop          = 0;
		tracee->ptrace_stop_reported = 0;
		if (tracee->ptrace_in_syscall) {
			tracee->ptrace_in_syscall = 0;
			task_wake(tracee);
		}
		return 0;

	default:
		return -ENOSYS;
	}
}

/* Read or write one word of the tracee's memory. */
static long ptrace_mem_req(process_t *p, process_t *tracee, long request, uint64_t addr, uint64_t data)
{
	switch (request) {
	case PTRACE_PEEKTEXT:
	case PTRACE_PEEKDATA: {
		if (tracee->tracer != (pid_t)p->pid) return -ESRCH;
		vm_space_t *vm = proc_get_vm((pid_t)tracee->pid);   /* pinned */
		if (!vm) return -ESRCH;
		uint64_t val = 0;
		size_t got = vm_access_remote(vm, addr, &val, sizeof(val), 0);
		vm_destroy(vm);
		if (got != sizeof(val)) return -EFAULT;
		/* the raw syscall stores through data; libc wrappers pass a local and
		 * return it, so a value left in the return register reads as garbage */
		if (!data) return (long)val;
		return copy_to_user((void *)data, &val, sizeof(val)) ? -14 : 0;
	}

	case PTRACE_POKETEXT:
	case PTRACE_POKEDATA: {
		if (tracee->tracer != (pid_t)p->pid) return -ESRCH;
		vm_space_t *vm = proc_get_vm((pid_t)tracee->pid);   /* pinned */
		if (!vm) return -ESRCH;
		size_t put = vm_access_remote(vm, addr, &data, sizeof(data), 1);
		vm_destroy(vm);
		return put == sizeof(data) ? 0 : -14;
	}

	default:
		return -ENOSYS;
	}
}

static long ptrace_req(process_t *p, process_t *tracee, long request, uint64_t addr, uint64_t data)
{
	switch (request) {
	case PTRACE_SETOPTIONS:
	case PTRACE_GETEVENTMSG:
	case PTRACE_GETSIGINFO:
	case PTRACE_SETSIGINFO:
		return ptrace_info_req(p, tracee, request, addr, data);

	case PTRACE_GETREGSET:
	case PTRACE_SETREGSET:
#if defined(__x86_64__)
	case PTRACE_GETFPREGS:
	case PTRACE_SETFPREGS:
	case PTRACE_PEEKUSER:
	case PTRACE_POKEUSER:
#endif
	case PTRACE_GETREGS:
	case PTRACE_SETREGS:
		return ptrace_regs_req(p, tracee, request, addr, data);

	case PTRACE_ATTACH:
	case PTRACE_DETACH:
	case PTRACE_CONT:
	case PTRACE_SYSCALL:
	case PTRACE_SINGLESTEP:
		return ptrace_run_req(p, tracee, request, addr, data);

	case PTRACE_PEEKTEXT:
	case PTRACE_PEEKDATA:
	case PTRACE_POKETEXT:
	case PTRACE_POKEDATA:
		return ptrace_mem_req(p, tracee, request, addr, data);

	default:
		return -ENOSYS;
	}
}

/* tracee is looked up by user-supplied pid; pin it (proc_get_user) across the whole
 * request so a concurrent exit+reap can't free it mid-operation. */
static long sys_ptrace(long request, pid_t pid, uint64_t addr, uint64_t data)
{
	process_t *p = task_current();

	if (request == PTRACE_TRACEME) {
		p->tracer    = p->ppid;
		p->ptrace_opts = 0;
		p->ptrace_options = 0;
		return 0;
	}

	process_t *tracee = proc_get_user(pid);
	if (!tracee)
		return -ESRCH;

	long ret = ptrace_req(p, tracee, request, addr, data);
	proc_put(tracee);
	return ret;
}
