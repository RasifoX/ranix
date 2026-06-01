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
	if (sig <= 0 || sig > NSIG || sigsetsize < 8) return -22;
	if (sig == SIGKILL) return -22;

	/* struct sigaction (kernel ABI): handler, flags, restorer, mask[N]
	 * Our sigaction_t mirrors this layout. User struct may have larger
	 * sa_mask (16 bytes for 128-bit RT sigset); we only store 64 bits. */
	size_t copy_sz = sizeof(sigaction_t);
	/* If user struct is larger (sigsetsize > 8), user's sa_mask is at offset
	 * 24..24+sigsetsize. We read/write only the first 8 bytes of sa_mask
	 * (offset 24..32 = sizeof sigaction_t). */

	process_t *p = task_current();
	if (oldact) {
		if (copy_to_user(oldact, &p->sig_actions[sig - 1], copy_sz) != 0)
			return -14;
	}
	if (act) {
		if (copy_from_user(&p->sig_actions[sig - 1], act, copy_sz) != 0)
			return -14;
	}
	return 0;
}

static long sys_rt_sigprocmask(int how, const uint64_t *set, uint64_t *oldset, size_t sigsetsize)
{
	if (sigsetsize < 8) return -22;
	process_t *p = task_current();
	if (oldset) {
		if (copy_to_user(oldset, &p->sig_mask, sizeof(uint64_t)) != 0)
			return -14;
	}
	if (set) {
		uint64_t new_set;
		if (copy_from_user(&new_set, set, sizeof(uint64_t)) != 0)
			return -14;

		if (how == 0) p->sig_mask |= new_set;        /* SIG_BLOCK */
		else if (how == 1) p->sig_mask &= ~new_set;  /* SIG_UNBLOCK */
		else if (how == 2) p->sig_mask = new_set;    /* SIG_SETMASK */
		else return -22;

		p->sig_mask &= ~(1UL << (SIGKILL - 1));
	}
	return 0;
}

static long sys_kill(pid_t pid, int sig)
{
	process_t *p = proc_find(pid);
	if (!p) return -3; /* ESRCH */
	task_signal(p, sig);
	return 0;
}

/* tgkill: simplified — thread groups not implemented, just route to kill */
static long sys_tgkill(pid_t tgid, pid_t tid, int sig)
{
	(void)tgid;
	return sys_kill(tid, sig);
}
