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
	if (sig == SIGKILL || sig == SIGSTOP) return -22;   /* uncatchable */

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
			return -14;
	}
	if (act) {
		if (copy_from_user(&p->sighand->actions[sig - 1], act, copy_sz) != 0)
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

		p->sig_mask &= ~((1UL << (SIGKILL - 1)) | (1UL << (SIGSTOP - 1)));  /* unblockable */
	}
	return 0;
}

static long sys_kill(pid_t pid, int sig)
{
	/* pid is namespace-local; resolve through the caller's pid namespace */
	process_t *p = pidns_find(current_pid_ns(), (int)pid);
	if (!p) return -3; /* ESRCH */
	task_signal_group(p, sig);   /* kill(2) is process-directed: any eligible thread */
	return 0;
}

/* tgkill: signal a specific thread (tid), but only if it still belongs to the
 * named thread group (tgid). Threads are individual process_t sharing a tgid,
 * so the tid resolves directly; the tgid check guards against a stale tid that
 * has exited and been reused by an unrelated process. */
static long sys_tgkill(pid_t tgid, pid_t tid, int sig)
{
	if (tgid <= 0 || tid <= 0) return -22; /* EINVAL */
	pid_namespace_t *ns = current_pid_ns();
	process_t *t = pidns_find(ns, (int)tid);
	if (!t) return -3; /* ESRCH */
	process_t *leader = pidns_find(ns, (int)tgid);
	if (!leader || t->tgid != leader->pid) return -3; /* tid not in tgid */
	task_signal(t, sig);
	return 0;
}

/* Return from a signal handler: restore the trap frame that signal_dispatch
 * saved on the user stack (full register set + the pre-signal mask), so the
 * interrupted code resumes exactly. Frame layout mirrors signal_dispatch. */
static void sys_rt_sigreturn(arch_context_t *ctx)
{
	process_t *p = task_current();
#if defined(__aarch64__)
	/* Restore full frame saved by signal_dispatch */
	uint64_t usp  = ctx->sp;
	ctx->elr      = *(uint64_t *)(usp +   0);
	ctx->regs[30] = *(uint64_t *)(usp +   8);
	p->sig_mask   = *(uint64_t *)(usp +  16);
	for (int _i = 0; _i <= 18; _i++)
		ctx->regs[_i] = *(uint64_t *)(usp + 32 + (uint64_t)_i * 8);
	ctx->regs[29] = *(uint64_t *)(usp + 184);
	/* PSTATE comes from the untrusted user stack — sanitize before it reaches
	 * `msr spsr_el1; eret`: keep only NZCV, forcing the rest to 0 (= EL0t, IRQs
	 * enabled, IL/SS clear). Without this a crafted frame with M=EL1h would eret
	 * straight into the kernel with user-controlled registers (local root). */
	ctx->spsr     = *(uint64_t *)(usp + 192) & 0xF0000000UL;
	ctx->fpsr     = *(uint32_t *)(usp + 200);
	ctx->fpcr     = *(uint32_t *)(usp + 204);
	for (int _i = 0; _i < 64; _i++)
		ctx->qregs[_i] = *(uint64_t *)(usp + 208 + (uint64_t)_i * 8);
	ctx->sp       = usp + 736;
#elif defined(__x86_64__)
	/* After handler ret + restorer syscall, ctx->rsp = frame_base+8.
	 * Frame layout matches signal_dispatch: offsets from frame_base+8. */
	uint64_t base = ctx->rsp;
	p->sig_mask   = *(uint64_t *)(base +   0);
	ctx->rip      = *(uint64_t *)(base +   8);
	ctx->rsp      = *(uint64_t *)(base +  16);
	/* RFLAGS comes from the untrusted user stack — keep only the user arithmetic
	 * flags (CF PF AF ZF SF TF DF OF) and force IF=1 / IOPL=0 / reserved bit, so
	 * a crafted frame can't run ring-3 with interrupts masked or grant itself I/O
	 * privilege (CS isn't restored, so no ring change, but the flags still matter). */
	ctx->rflags   = (*(uint64_t *)(base +  24) & 0x00000CD5UL) | 0x202UL;
	ctx->rax      = *(uint64_t *)(base +  32);
	ctx->rbx      = *(uint64_t *)(base +  40);
	ctx->rcx      = *(uint64_t *)(base +  48);
	ctx->rdx      = *(uint64_t *)(base +  56);
	ctx->rsi      = *(uint64_t *)(base +  64);
	ctx->rdi      = *(uint64_t *)(base +  72);
	ctx->rbp      = *(uint64_t *)(base +  80);
	ctx->r8       = *(uint64_t *)(base +  88);
	ctx->r9       = *(uint64_t *)(base +  96);
	ctx->r10      = *(uint64_t *)(base + 104);
	ctx->r11      = *(uint64_t *)(base + 112);
	ctx->r12      = *(uint64_t *)(base + 120);
	ctx->r13      = *(uint64_t *)(base + 128);
	ctx->r14      = *(uint64_t *)(base + 136);
	ctx->r15      = *(uint64_t *)(base + 144);
	memcpy(ctx->fxsave, (void *)(base + 160), sizeof(ctx->fxsave));
#endif
}
