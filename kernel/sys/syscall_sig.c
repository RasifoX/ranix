/* ── signal-management syscalls ──────────────────────────────────────────────
 * Installing handlers (rt_sigaction), blocking/unblocking signals
 * (rt_sigprocmask), and sending them (kill/tgkill). The actual delivery — where
 * a pending unmasked signal redirects a returning trap frame to its handler or
 * applies the default action — lives in signal_dispatch (kernel/sched.c); the
 * frame restore after a handler returns is sys_rt_sigreturn (kernel/syscall.c).
 *
 * Compiled as part of syscall.c (textual include): NSIG, SIGKILL and
 * sigaction_t are already in scope.
 * ───────────────────────────────────────────────────────────────────────── */

static long sys_rt_sigaction(int sig, const sigaction_t *act, sigaction_t *oldact, size_t sigsetsize)
{
	if (sig <= 0 || sig > NSIG || sigsetsize < 8) return -EINVAL;
	if (sig == SIGKILL || sig == SIGSTOP) return -EINVAL;   /* uncatchable */

	/* struct sigaction (kernel ABI): handler, flags, restorer, mask[N]
	 * Our sigaction_t mirrors this layout. User struct may have larger
	 * sa_mask (16 bytes for 128-bit RT sigset); we only store 64 bits. */
	size_t copy_sz = sizeof(sigaction_t);
	/* If user struct is larger (sigsetsize > 8), user's sa_mask is at offset
	 * 24..24+sigsetsize. We read/write only the first 8 bytes of sa_mask
	 * (offset 24..32 = sizeof sigaction_t). */

	process_t *p = task_current();
	if (oldact) {
		if (copy_to_user(oldact, &p->sighand->actions[sig - 1], copy_sz) != 0)
			return -EFAULT;
	}
	if (act) {
		if (copy_from_user(&p->sighand->actions[sig - 1], act, copy_sz) != 0)
			return -EFAULT;
	}
	return 0;
}

static long sys_rt_sigprocmask(int how, const uint64_t *set, uint64_t *oldset, size_t sigsetsize)
{
	if (sigsetsize < 8) return -EINVAL;
	process_t *p = task_current();
	if (oldset) {
		if (copy_to_user(oldset, &p->sig_mask, sizeof(uint64_t)) != 0)
			return -EFAULT;
	}
	if (set) {
		uint64_t new_set;
		if (copy_from_user(&new_set, set, sizeof(uint64_t)) != 0)
			return -EFAULT;

		if (how == 0) p->sig_mask |= new_set;        /* SIG_BLOCK */
		else if (how == 1) p->sig_mask &= ~new_set;  /* SIG_UNBLOCK */
		else if (how == 2) p->sig_mask = new_set;    /* SIG_SETMASK */
		else return -EINVAL;

		p->sig_mask &= ~((1UL << (SIGKILL - 1)) | (1UL << (SIGSTOP - 1)));  /* unblockable */
	}
	return 0;
}

/* Process-directed signal to a pinned task. */
static long kill_task(process_t *t, int sig)
{
	if (sig < 0 || sig > NSIG) return -EINVAL;
	if (!signal_may_send(task_current(), t, sig)) return -EPERM;
	if (sig) task_signal_group(t, sig);   /* any eligible thread */
	return 0;
}

static long sys_kill(pid_t pid, int sig)
{
	if (sig < 0 || sig > NSIG) return -EINVAL;
	process_t *cur = task_current();

	if (pid > 0) {
		process_t *p = proc_get_user(pid);
		if (!p) return -ESRCH;
		long r = kill_task(p, sig);
		proc_put(p);
		return r;
	}

	if (pid == -1) return task_kill_many(cur, 0, sig);

	/* pid==0 → caller's group; pid<-1 → group -pid. The stored pgrp is a global
	 * pid, so map the supplied ns-local number to it. */
	pid_t pgrp = pid ? pid_from_user(-pid) : cur->pgrp;
	if (!pgrp) return -ESRCH;
	return task_kill_many(cur, pgrp, sig);
}

/* tgkill: signal a specific thread (tid), but only if it still belongs to the
 * named thread group (tgid). Threads are individual process_t sharing a tgid,
 * so the tid resolves directly; the tgid check guards against a stale tid that
 * has exited and been reused by an unrelated process. */
static long sys_tgkill(pid_t tgid, pid_t tid, int sig)
{
	if (tgid <= 0 || tid <= 0 || sig < 0 || sig > NSIG) return -EINVAL;
	process_t *t = proc_get_user(tid);
	process_t *leader = proc_get_user(tgid);
	long r = 0;
	if (!t || !leader || t->tgid != leader->pid) r = -3; /* tid not in tgid */
	else if (!signal_may_send(task_current(), t, sig)) r = -1; /* EPERM */
	else if (sig) task_signal(t, sig);   /* 0 only probes; 1 << -1 is no signal */
	proc_put(leader);
	proc_put(t);
	return r;
}

/* tkill: this thread and no other. pthread_kill and raise rely on it: QEMU blocks a
 * signal, sends it to itself and waits for it, and a sibling that took it instead
 * leaves the sender asleep. */
static long sys_tkill(pid_t tid, int sig)
{
	if (tid <= 0 || sig < 0 || sig > NSIG) return -EINVAL;
	process_t *t = proc_get_user(tid);
	if (!t) return -ESRCH;
	long r = 0;
	if (!signal_may_send(task_current(), t, sig)) r = -1; /* EPERM */
	else if (sig) task_signal(t, sig);
	proc_put(t);
	return r;
}

/* sigwait, sigwaitinfo, sigtimedwait: take a signal out of the pending set instead
 * of delivering it. The set is unblocked while asleep so a poster wakes this task;
 * the loop dequeues before user mode is reached, so no handler runs. */
static long sys_rt_sigtimedwait(const uint64_t *uset, void *uinfo, const linux_timespec_t *uto, size_t sz)
{
	if (sz != 8) return -EINVAL;
	uint64_t set;
	if (copy_from_user(&set, uset, sizeof(set)) != 0) return -EFAULT;
	set &= ~((1UL << (SIGKILL - 1)) | (1UL << (SIGSTOP - 1)));
	uint64_t end = 0;
	if (uto) {
		linux_timespec_t k;
		if (copy_from_user(&k, uto, sizeof(k)) != 0) return -EFAULT;
		if (k.tv_sec < 0 || k.tv_nsec < 0 || k.tv_nsec >= 1000000000L) return -EINVAL;
		uint64_t t = (uint64_t)k.tv_sec * TIMER_HZ + (uint64_t)k.tv_nsec / (1000000000 / TIMER_HZ);
		end = kernel_ticks + (t || !k.tv_nsec ? t : 1);
	}
	process_t *p = task_current();
	uint64_t blocked = p->sig_mask;
	long ret;
	for (;;) {
		sigqueue_t si;
		int have = 0, sig = 0;
		sig = task_dequeue_signal(p, set, &si, &have);
		p->sig_mask = blocked & ~set;
		if (sig) {
			ret = sig;
			if (uinfo) {
				int32_t info[32];
				memset(info, 0, sizeof(info));
				info[0] = sig;
				if (have) {
					info[2] = si.si_code;
					info[4] = (int32_t)si.si_pid;
					info[5] = (int32_t)si.si_uid;
					memcpy(&info[6], &si.si_value, 8);
				}
				if (copy_to_user(uinfo, info, sizeof(info)) != 0) ret = -14;
			}
			break;
		}
		if (task_has_actionable_signal()) { ret = -4; break; }
		if (uto && kernel_ticks >= end) { ret = -11; break; }
		task_sleep_until(uto ? end : (uint64_t)-1 / 2);
	}
	p->sig_mask = blocked;
	return ret;
}

/* sigqueue(2): post an RT signal to a process carrying si_value. The siginfo
 * the sender supplies is read from user memory; we only trust its si_code and
 * si_value and stamp the real sender pid/uid. */
static long sys_rt_sigqueueinfo(pid_t pid, int sig, const void *uinfo)
{
	if (sig <= 0 || sig > NSIG) return -EINVAL;
	int32_t  si_code = 0;
	uint64_t si_value = 0;
	if (copy_from_user(&si_code, (const uint8_t *)uinfo + 8, sizeof(si_code)) != 0) return -EFAULT;
	if (copy_from_user(&si_value, (const uint8_t *)uinfo + 24, sizeof(si_value)) != 0) return -EFAULT;
	process_t *p = proc_get_user(pid);
	if (!p) return -ESRCH;
	process_t *cur = task_current();
	long r = signal_may_send(cur, p, sig)
	       ? task_signal_info(p, sig, si_code, (pid_t)cur->pid, cur->euid, si_value, 0) : -1;
	proc_put(p);
	return r;
}

static long sys_rt_tgsigqueueinfo(pid_t tgid, pid_t tid, int sig, const void *uinfo)
{
	if (tgid <= 0 || tid <= 0 || sig <= 0 || sig > NSIG) return -EINVAL;
	int32_t  si_code = 0;
	uint64_t si_value = 0;
	if (copy_from_user(&si_code, (const uint8_t *)uinfo + 8, sizeof(si_code)) != 0) return -EFAULT;
	if (copy_from_user(&si_value, (const uint8_t *)uinfo + 24, sizeof(si_value)) != 0) return -EFAULT;
	process_t *t = proc_get_user(tid);
	process_t *leader = proc_get_user(tgid);
	process_t *cur = task_current();
	long r;
	if (!t || !leader || t->tgid != leader->pid) r = -3;
	else if (!signal_may_send(cur, t, sig)) r = -1;
	else r = task_signal_info(t, sig, si_code, (pid_t)cur->pid, cur->euid, si_value, 1);
	proc_put(leader);
	proc_put(t);
	return r;
}

/* Return from a signal handler: restore the trap frame that signal_dispatch
 * saved on the user stack (full register set + the pre-signal mask), so the
 * interrupted code resumes exactly. Frame layout mirrors signal_dispatch. The
 * frame sits wherever the user's sp points, so it is staged through the checked
 * copy; a bad one is a SIGSEGV. */
static void sys_rt_sigreturn(arch_context_t *ctx)
{
	process_t *p = task_current();
#if defined(__aarch64__)
	/* Restore full frame saved by signal_dispatch */
	uint64_t usp  = ctx->sp;
	uint8_t kframe[736];
	if (copy_from_user(kframe, (void *)usp, sizeof(kframe)) != 0) {
		p->exit_code = 11; p->killed_by_sig = 1;
		task_exit();
	}
	uint64_t kf = (uint64_t)kframe;
	ctx->elr      = *(uint64_t *)(kf +   0);
	ctx->regs[30] = *(uint64_t *)(kf +   8);
	p->sig_mask   = *(uint64_t *)(kf +  16) & ~((1UL << (SIGKILL - 1)) | (1UL << (SIGSTOP - 1)));
	for (int _i = 0; _i <= 18; _i++)
		ctx->regs[_i] = *(uint64_t *)(kf + 32 + (uint64_t)_i * 8);
	ctx->regs[29] = *(uint64_t *)(kf + 184);
	/* PSTATE comes from the untrusted user stack — sanitize before it reaches
	 * `msr spsr_el1; eret`: keep only NZCV, forcing the rest to 0 (= EL0t, IRQs
	 * enabled, IL/SS clear). Without this a crafted frame with M=EL1h would eret
	 * straight into the kernel with user-controlled registers (local root). */
	ctx->spsr     = *(uint64_t *)(kf + 192) & 0xF0000000UL;
	ctx->fpsr     = *(uint32_t *)(kf + 200);
	ctx->fpcr     = *(uint32_t *)(kf + 204);
	for (int _i = 0; _i < 64; _i++)
		ctx->qregs[_i] = *(uint64_t *)(kf + 208 + (uint64_t)_i * 8);
	ctx->sp       = usp + 864;
#elif defined(__x86_64__)
	/* After handler ret + restorer syscall, ctx->rsp = frame_base+8.
	 * Frame layout matches signal_dispatch: offsets from frame_base+8. */
	uint64_t base = ctx->rsp;
	uint8_t kframe[160 + sizeof(ctx->fxsave)] __attribute__((aligned(16)));
	if (copy_from_user(kframe, (void *)base, sizeof(kframe)) != 0) {
		p->exit_code = 11; p->killed_by_sig = 1;
		task_exit();
	}
	if (copy_from_user(ctx->ymmh, (void *)(base + 1232), sizeof(ctx->ymmh)) != 0) {
		p->exit_code = 11; p->killed_by_sig = 1;
		task_exit();
	}
	uint64_t kf = (uint64_t)kframe;
	p->sig_mask   = *(uint64_t *)(kf +   0) & ~((1UL << (SIGKILL - 1)) | (1UL << (SIGSTOP - 1)));
	ctx->rip      = *(uint64_t *)(kf +   8);
	ctx->rsp      = *(uint64_t *)(kf +  16);
	/* RFLAGS comes from the untrusted user stack — keep only the user arithmetic
	 * flags (CF PF AF ZF SF TF DF OF) and force IF=1 / IOPL=0 / reserved bit, so
	 * a crafted frame can't run ring-3 with interrupts masked or grant itself I/O
	 * privilege (CS isn't restored, so no ring change, but the flags still matter). */
	ctx->rflags   = (*(uint64_t *)(kf +  24) & 0x00000CD5UL) | 0x202UL;
	ctx->rax      = *(uint64_t *)(kf +  32);
	ctx->rbx      = *(uint64_t *)(kf +  40);
	ctx->rcx      = *(uint64_t *)(kf +  48);
	ctx->rdx      = *(uint64_t *)(kf +  56);
	ctx->rsi      = *(uint64_t *)(kf +  64);
	ctx->rdi      = *(uint64_t *)(kf +  72);
	ctx->rbp      = *(uint64_t *)(kf +  80);
	ctx->r8       = *(uint64_t *)(kf +  88);
	ctx->r9       = *(uint64_t *)(kf +  96);
	ctx->r10      = *(uint64_t *)(kf + 104);
	ctx->r11      = *(uint64_t *)(kf + 112);
	ctx->r12      = *(uint64_t *)(kf + 120);
	ctx->r13      = *(uint64_t *)(kf + 128);
	ctx->r14      = *(uint64_t *)(kf + 136);
	ctx->r15      = *(uint64_t *)(kf + 144);
	memcpy(ctx->fxsave, (void *)(kf + 160), sizeof(ctx->fxsave));
	/* fxrstor runs in ring 0 and #GPs on a reserved MXCSR bit */
	*(uint32_t *)&ctx->fxsave[24] &= 0xffff;
	/* A handler edits the ucontext it was given: a JIT moves rip past a trapping
	 * instruction, Go injects a call. That copy is what the program sees as its
	 * registers, so it wins over the frame's. Frame base + 808, as in signal.c. */
	uint64_t g[19], umask;
	if (copy_from_user(g, (void *)(base - 8 + 808 + 40), sizeof(g)) != 0 ||
	    copy_from_user(&umask, (void *)(base - 8 + 808 + 296), sizeof(umask)) != 0 ||
	    g[18] != 0x33)
		return;   /* no ucontext: a frame built by hand, not by delivery */
	ctx->r8  = g[0];  ctx->r9  = g[1];  ctx->r10 = g[2];  ctx->r11 = g[3];
	ctx->r12 = g[4];  ctx->r13 = g[5];  ctx->r14 = g[6];  ctx->r15 = g[7];
	ctx->rdi = g[8];  ctx->rsi = g[9];  ctx->rbp = g[10]; ctx->rbx = g[11];
	ctx->rdx = g[12]; ctx->rax = g[13]; ctx->rcx = g[14]; ctx->rsp = g[15];
	ctx->rip = g[16];
	ctx->rflags = (g[17] & 0x00000CD5UL) | 0x202UL;
	p->sig_mask = umask & ~((1UL << (SIGKILL - 1)) | (1UL << (SIGSTOP - 1)));
#endif
}
