/* ── signal delivery ─────────────────────────────────────────────────────────
 * The kernel side of POSIX signals: posting a signal to a task (task_signal /
 * task_signal_pgrp), deciding whether a pending signal is actionable, and
 * delivering it on the way back to user space (signal_dispatch) — either by
 * redirecting the trap frame to the handler (saving the full register + FP
 * state so the interrupted code resumes intact) or by applying the default
 * action. Handler installation/masking is in sys/syscall_sig.c; the return
 * trampoline is sys_rt_sigreturn there.
 *
 * Compiled as part of sched.c (textual include): the scheduler statics
 * (sched_lock, proctab, runqueue_add) and task_exit are already in scope.
 * ───────────────────────────────────────────────────────────────────────── */

void task_signal(process_t *p, int sig)
{
	if (!p || sig <= 0 || sig > NSIG) return;
	uint64_t f = spin_lock_irq(&sched_lock);
	p->sig_pending |= (1UL << (sig - 1));
	if (p->state == PROC_SLEEPING) {
		p->state = PROC_READY;
		if (!p->on_cpu) runqueue_add(p);
		arch_get_cpu_ctx()->need_resched = 1;
	}
	spin_unlock_irq(&sched_lock, f);
}

/* Process-directed signal (kill(2)): POSIX delivers it to *any one* thread of
 * the group that doesn't have the signal blocked — not necessarily the leader.
 * Prefer the leader when it isn't blocking (the common single-threaded case, so
 * behaviour is unchanged there); otherwise hand it to the first eligible thread.
 * If every thread blocks it, leave it pending on the leader. */
void task_signal_group(process_t *leader, int sig)
{
	if (!leader || sig <= 0 || sig > NSIG) return;
	uint64_t bit = 1UL << (sig - 1);
	uint64_t f = spin_lock_irq(&sched_lock);

	process_t *target = NULL;
	if (leader->state != PROC_DEAD && leader->state != PROC_ZOMBIE && !(leader->sig_mask & bit))
		target = leader;
	if (!target) {
		process_t *sentinel = proctab[0];
		for (process_t *t = sentinel->next; t && t != sentinel; t = t->next) {
			if (t->tgid == leader->tgid && t->state != PROC_DEAD && t->state != PROC_ZOMBIE
			    && !(t->sig_mask & bit)) { target = t; break; }
		}
	}
	if (!target) target = leader;   /* nobody can take it now → pend on the leader */

	target->sig_pending |= bit;
	if (target->state == PROC_SLEEPING) {
		target->state = PROC_READY;
		if (!target->on_cpu) runqueue_add(target);
		arch_get_cpu_ctx()->need_resched = 1;
	}
	spin_unlock_irq(&sched_lock, f);
}

void task_signal_pgrp(pid_t pgrp, int sig)
{
	uint64_t f = spin_lock_irq(&sched_lock);
	process_t *sentinel = proctab[0];
	for (process_t *t = sentinel->next; t && t != sentinel; t = t->next) {
		if (t->pgrp == pgrp && t->state != PROC_DEAD && t->state != PROC_ZOMBIE)
			t->sig_pending |= (1UL << (sig - 1));
	}
	spin_unlock_irq(&sched_lock, f);
}

/* Signals that kill the process by default */
static int sig_default_kill(int sig)
{
	switch (sig) {
	case 1:  /* SIGHUP  */
	case 2:  /* SIGINT  */
	case 3:  /* SIGQUIT */
	case 6:  /* SIGABRT */
	case 8:  /* SIGFPE  */
	case 9:  /* SIGKILL */
	case 11: /* SIGSEGV */
	case 13: /* SIGPIPE */
	case 14: /* SIGALRM */
	case 15: /* SIGTERM */
	case 26: /* SIGVTALRM */
	case 27: /* SIGPROF */
		return 1;
	default:
		return 0;
	}
}

int task_has_actionable_signal(void)
{
	process_t *p = task_current();
	uint64_t mask = p->sig_pending & ~p->sig_mask;
	while (mask) {
		int sig = __builtin_ctzll(mask) + 1;
		mask &= mask - 1;
		void (*handler)(int) = p->sighand->actions[sig - 1].sa_handler;
		if ((uintptr_t)handler == 1) continue;   /* SIG_IGN */
		if (!handler && !sig_default_kill(sig)) continue; /* SIG_DFL non-fatal */
		return 1;
	}
	return 0;
}

void signal_dispatch(arch_context_t *tf)
{
	arch_get_cpu_ctx()->wd_where = "signal_dispatch";
	/* Only deliver signals when returning to user space */
#if defined(__x86_64__)
	if ((tf->cs & 3) != 3) return;
#elif defined(__aarch64__)
	if ((tf->spsr & 0xF) != 0) return;
#endif
	process_t *p = task_current();
	if (!p) return;

	/* Catch a kernel-stack overflow in the same trap that caused it. */
	extern void kstack_check(void);
	kstack_check();

	/* (Re)arm single-step TF on this user return, independent of signals. */
	extern void ptrace_apply_singlestep(arch_context_t *);
	ptrace_apply_singlestep(tf);

	uint64_t mask = p->sig_pending & ~p->sig_mask;
	if (!mask) return;

	int sig = 0;
	for (int i = 0; i < NSIG; i++) {
		if (mask & (1UL << i)) { sig = i + 1; break; }
	}
	if (!sig) return;

	p->sig_pending &= ~(1UL << (sig - 1));

	sigaction_t *sa = &p->sighand->actions[sig - 1];
	void (*handler)(int) = sa->sa_handler;

	/* SIG_IGN = 1 */
	if ((uintptr_t)handler == 1) return;

	/* SIG_DFL = 0 or NULL */
	if (!handler) {
		if (sig_default_kill(sig)) {
			p->exit_code      = sig;
			p->killed_by_sig  = 1;
			extern void coredump(process_t *, int, arch_context_t *);
			coredump(p, sig, tf);
			arch_get_cpu_ctx()->wd_where = "sig:kill";
			task_exit();
		}
		return;
	}
	arch_get_cpu_ctx()->wd_where = "sig:frame";

	/* Deliver to user handler: redirect the trap frame */
	/* Block this signal during handler execution; save old mask for restore */
	uint64_t old_mask = p->sig_mask;
	p->sig_mask |= (1UL << (sig - 1));
	p->sig_mask |= sa->sa_mask;

#if defined(__aarch64__)
	/* Frame layout — 736 bytes, 16-byte aligned:
	 *   [sp+  0]  orig_elr  (PC to resume)
	 *   [sp+  8]  orig_x30  (LR)
	 *   [sp+ 16]  old_mask
	 *   [sp+ 24]  padding
	 *   [sp+ 32]  orig_x0 .. orig_x18  (19 caller-saved + indirect-result regs)
	 *   [sp+184]  orig_x29 (frame pointer)
	 *   [sp+192]  orig_spsr
	 *   [sp+200]  orig_fpsr (4) + orig_fpcr (4)
	 *   [sp+208]  orig_q0 .. q31  (512 bytes)
	 *   [sp+720]  padding × 2  (→ total 736)
	 *
	 * The FP/SIMD state MUST round-trip: musl's memcpy/strlen are vectorised,
	 * so a signal landing mid-copy whose handler clobbers q-regs would resume
	 * the interrupted op with garbage SIMD state and corrupt memory silently.
	 */
	uint64_t usp = (tf->sp - 736) & ~15UL;
	/* If the user stack can't hold the frame (bad/overflowed sp), writing it
	 * would fault in EL1 and panic. Linux force_sigsegv's the task instead. */
	if (!access_ok((void *)usp, 736) || !user_pages_mapped((void *)usp, 736)) {
		p->exit_code = 11; p->killed_by_sig = 1;
		arch_get_cpu_ctx()->wd_where = "sig:badframe";
		task_exit();
	}
	*(uint64_t *)(usp +   0) = tf->elr;
	*(uint64_t *)(usp +   8) = tf->regs[30];
	*(uint64_t *)(usp +  16) = old_mask;
	*(uint64_t *)(usp +  24) = 0;
	for (int _i = 0; _i <= 18; _i++)
		*(uint64_t *)(usp + 32 + (uint64_t)_i * 8) = tf->regs[_i];
	*(uint64_t *)(usp + 184) = tf->regs[29];
	*(uint64_t *)(usp + 192) = tf->spsr;
	*(uint32_t *)(usp + 200) = tf->fpsr;
	*(uint32_t *)(usp + 204) = tf->fpcr;
	for (int _i = 0; _i < 64; _i++)
		*(uint64_t *)(usp + 208 + (uint64_t)_i * 8) = tf->qregs[_i];
	tf->sp                   = usp;
	tf->regs[30]             = sa->sa_restorer ? (uint64_t)sa->sa_restorer
	                                            : tf->elr;
	tf->regs[0]              = (uint64_t)sig;
	tf->elr                  = (uint64_t)handler;
#elif defined(__x86_64__)
	/* Frame layout — 168 bytes, rsp kept 8 mod 16 (post-call ABI convention):
	 *   [rsp+  0]  return addr (sa_restorer → rt_sigreturn)
	 *   [rsp+  8]  old_mask
	 *   [rsp+ 16]  orig_rip
	 *   [rsp+ 24]  orig_rsp
	 *   [rsp+ 32]  orig_rflags
	 *   [rsp+ 40]  orig_rax
	 *   [rsp+ 48]  orig_rbx
	 *   [rsp+ 56]  orig_rcx
	 *   [rsp+ 64]  orig_rdx
	 *   [rsp+ 72]  orig_rsi
	 *   [rsp+ 80]  orig_rdi
	 *   [rsp+ 88]  orig_rbp
	 *   [rsp+ 96]  orig_r8
	 *   [rsp+104]  orig_r9
	 *   [rsp+112]  orig_r10
	 *   [rsp+120]  orig_r11
	 *   [rsp+128]  orig_r12
	 *   [rsp+136]  orig_r13
	 *   [rsp+144]  orig_r14
	 *   [rsp+152]  orig_r15
	 *   [rsp+160]  padding × 1
	 *   [rsp+168]  fxsave area (512 bytes, 16-byte aligned)  (→ total 680)
	 * rt_sigreturn reads base = ctx->rsp (= frame_base+8 after handler ret).
	 * The FP/SSE state must round-trip or a handler's xmm use corrupts the
	 * interrupted code's vector registers (same class as the aarch64 bug). */
	uint64_t usp = (tf->rsp & ~15UL) - 680; /* 8-mod-16 entry; usp+168 is 16-aligned */
	/* Bad/overflowed user sp: writing the frame would fault in the kernel and
	 * panic. Force-kill with SIGSEGV like Linux instead. */
	if (!access_ok((void *)usp, 680) || !user_pages_mapped((void *)usp, 680)) {
		p->exit_code = 11; p->killed_by_sig = 1;
		arch_get_cpu_ctx()->wd_where = "sig:badframe";
		task_exit();
	}
	*(uint64_t *)(usp +   0) = sa->sa_restorer ? (uint64_t)sa->sa_restorer : tf->rip;
	*(uint64_t *)(usp +   8) = old_mask;
	*(uint64_t *)(usp +  16) = tf->rip;
	*(uint64_t *)(usp +  24) = tf->rsp;
	*(uint64_t *)(usp +  32) = tf->rflags;
	*(uint64_t *)(usp +  40) = tf->rax;
	*(uint64_t *)(usp +  48) = tf->rbx;
	*(uint64_t *)(usp +  56) = tf->rcx;
	*(uint64_t *)(usp +  64) = tf->rdx;
	*(uint64_t *)(usp +  72) = tf->rsi;
	*(uint64_t *)(usp +  80) = tf->rdi;
	*(uint64_t *)(usp +  88) = tf->rbp;
	*(uint64_t *)(usp +  96) = tf->r8;
	*(uint64_t *)(usp + 104) = tf->r9;
	*(uint64_t *)(usp + 112) = tf->r10;
	*(uint64_t *)(usp + 120) = tf->r11;
	*(uint64_t *)(usp + 128) = tf->r12;
	*(uint64_t *)(usp + 136) = tf->r13;
	*(uint64_t *)(usp + 144) = tf->r14;
	*(uint64_t *)(usp + 152) = tf->r15;
	*(uint64_t *)(usp + 160) = 0;
	memcpy((void *)(usp + 168), tf->fxsave, sizeof(tf->fxsave));  /* 16-aligned */
	tf->rsp                  = usp;
	tf->rdi                  = (uint64_t)sig;
	tf->rip                  = (uint64_t)handler;
#endif
}
