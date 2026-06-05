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

static int sig_default_stop(int sig);

/* Wake a target that may be blocked so a freshly-posted signal is delivered.
 * Caller holds sched_lock. */
static void sig_wake_locked(process_t *t)
{
	if (t->state == PROC_SLEEPING) {
		t->state = PROC_READY;
		if (!t->on_cpu) runqueue_add(t);
		arch_get_cpu_ctx()->need_resched = 1;
	}
}

/* Append a queued siginfo to t, keeping FIFO within a signo and the list sorted
 * by signo so the dispatcher pops lowest-first. Caller holds sched_lock. */
static void sigq_append_locked(process_t *t, sigqueue_t *q)
{
	sigqueue_t **pp = &t->sigq_head;
	while (*pp && (*pp)->si_signo <= q->si_signo) pp = &(*pp)->next;
	q->next = *pp;
	*pp = q;
	if (!q->next) t->sigq_tail = q;
	t->sigq_count++;
}

/* Pop the oldest queued siginfo for `sig` (head is lowest-signo, FIFO within).
 * Caller holds sched_lock. Returns 1 and fills *out if found. */
static int sigq_pop_locked(process_t *p, int sig, sigqueue_t *out)
{
	sigqueue_t **pp = &p->sigq_head, *prev = NULL;
	for (sigqueue_t *q = p->sigq_head; q; prev = q, q = q->next) {
		if (q->si_signo == sig) {
			*pp = q->next;
			if (p->sigq_tail == q) p->sigq_tail = prev;
			p->sigq_count--;
			*out = *q;
			kfree(q);
			return 1;
		}
		pp = &q->next;
	}
	return 0;
}

/* Any more queued instances of `sig` after a pop? Caller holds sched_lock. */
static int sigq_has_locked(process_t *p, int sig)
{
	for (sigqueue_t *q = p->sigq_head; q; q = q->next)
		if (q->si_signo == sig) return 1;
	return 0;
}

void sigqueue_drop(process_t *p)
{
	uint64_t f = spin_lock_irq(&sched_lock);
	sigqueue_t *q = p->sigq_head;
	p->sigq_head = p->sigq_tail = NULL;
	p->sigq_count = 0;
	spin_unlock_irq(&sched_lock, f);
	while (q) { sigqueue_t *n = q->next; kfree(q); q = n; }
}

#define STOP_SIG_MASK ((1UL << (SIGSTOP - 1)) | (1UL << (SIGTSTP - 1)) | \
                       (1UL << (SIGTTIN - 1)) | (1UL << (SIGTTOU - 1)))

/* POSIX: SIGCONT and the stop signals cancel each other when posted. SIGCONT
 * also resumes a job-stopped task immediately so it can run to deliver. Caller
 * holds sched_lock. */
static void sig_cont_stop_locked(process_t *t, int sig)
{
	if (sig == SIGCONT) {
		t->sig_pending &= ~STOP_SIG_MASK;
		if (t->job_stopped) {
			t->job_stopped = 0;
			t->job_continued = 1;
		}
	} else if (sig_default_stop(sig)) {
		t->sig_pending &= ~(1UL << (SIGCONT - 1));
	}
}

void task_signal(process_t *p, int sig)
{
	if (!p || sig <= 0 || sig > NSIG) return;
	uint64_t f = spin_lock_irq(&sched_lock);
	p->sig_pending |= (1UL << (sig - 1));
	sig_cont_stop_locked(p, sig);
	sig_wake_locked(p);
	spin_unlock_irq(&sched_lock, f);
}

int task_signal_info(process_t *p, int sig, int si_code, pid_t si_pid,
                     uid_t si_uid, uint64_t si_value, int thread_directed)
{
	if (!p || sig <= 0 || sig > NSIG) return -22;

	/* Standard signals coalesce — the bitmask is sufficient and the siginfo is
	 * synthesized on delivery. Only RT signals queue. */
	int is_rt = (sig >= SIGRTMIN && sig <= SIGRTMAX);

	sigqueue_t *q = NULL;
	if (is_rt) {
		q = kmalloc(sizeof(*q));
		if (!q) return -11; /* EAGAIN */
		q->next = NULL;
		q->si_signo = sig;
		q->si_errno = 0;
		q->si_code  = si_code;
		q->si_pid   = si_pid;
		q->si_uid   = si_uid;
		q->si_value = si_value;
	}

	uint64_t f = spin_lock_irq(&sched_lock);

	process_t *target = p;
	if (!thread_directed) {
		/* process-directed: pick an eligible thread, mirroring task_signal_group */
		uint64_t bit = 1UL << (sig - 1);
		target = NULL;
		if (p->state != PROC_DEAD && p->state != PROC_ZOMBIE && !(p->sig_mask & bit))
			target = p;
		if (!target) {
			process_t *sentinel = proctab[0];
			for (process_t *t = sentinel->next; t && t != sentinel; t = t->next) {
				if (t->tgid == p->tgid && t->state != PROC_DEAD && t->state != PROC_ZOMBIE
				    && !(t->sig_mask & bit)) { target = t; break; }
			}
		}
		if (!target) target = p;
	}

	if (is_rt) {
		if (target->sigq_count >= SIGQUEUE_MAX) {
			spin_unlock_irq(&sched_lock, f);
			kfree(q);
			return -11; /* EAGAIN */
		}
		sigq_append_locked(target, q);
	}
	target->sig_pending |= (1UL << (sig - 1));
	sig_cont_stop_locked(target, sig);
	sig_wake_locked(target);
	spin_unlock_irq(&sched_lock, f);
	return 0;
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
	sig_cont_stop_locked(target, sig);
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
		if (t->pgrp == pgrp && t->state != PROC_DEAD && t->state != PROC_ZOMBIE) {
			t->sig_pending |= (1UL << (sig - 1));
			sig_cont_stop_locked(t, sig);
			/* wake a blocked member so the signal is actually delivered — Ctrl-C/
			 * Ctrl-Z to a foreground job stuck in a blocking syscall must fire.
			 * (Linux: signal_wake_up.) */
			if (t->state == PROC_SLEEPING) {
				t->state = PROC_READY;
				if (!t->on_cpu) runqueue_add(t);
				arch_get_cpu_ctx()->need_resched = 1;
			}
		}
	}
	spin_unlock_irq(&sched_lock, f);
}

/* Signals whose default action stops the process (job control) */
static int sig_default_stop(int sig)
{
	return sig == SIGSTOP || sig == SIGTSTP || sig == SIGTTIN || sig == SIGTTOU;
}

/* Stop the current task on signal `sig` (SIGSTOP/TSTP/TTIN/TTOU): publish the
 * job-control stop, wake the parent's waitpid(WUNTRACED) poll, and sleep until
 * SIGCONT clears job_stopped (SIGKILL also breaks out, to terminate). */
static void job_stop_self(int sig)
{
	process_t *p = task_current();
	uint64_t sf = spin_lock_irq(&sched_lock);
	p->job_stopped       = 1;
	p->job_stop_sig      = sig;
	p->job_stop_reported = 0;
	spin_unlock_irq(&sched_lock, sf);
	process_t *par = proc_find(p->ppid);
	if (par) task_wake(par);
	arch_get_cpu_ctx()->wd_where = "sig:stop";
	while (p->job_stopped && !(p->sig_pending & (1UL << (SIGKILL - 1))))
		task_sleep_interruptible();
	p->job_stopped = 0;
}

/* A pgrp is orphaned if no member has a parent in a different pgrp of the same
 * session. Background I/O from an orphaned pgrp returns EIO instead of stopping
 * (there is no job-control shell that could ever continue it). Caller holds
 * sched_lock. */
static int pgrp_is_orphaned_locked(pid_t pgrp, pid_t sid)
{
	process_t *sentinel = proctab[0];
	for (process_t *t = sentinel->next; t && t != sentinel; t = t->next) {
		if (t->pgrp != pgrp || t->state == PROC_DEAD || t->state == PROC_ZOMBIE)
			continue;
		process_t *par = (t->ppid > 0 && t->ppid < proc_cnt) ? proctab[t->ppid] : NULL;
		if (par && par->pgrp != pgrp && par->sid == sid)
			return 0;
	}
	return 1;
}

/* Background-pgrp tty I/O (SIG-3): the caller's pgrp differs from the tty's
 * foreground group. Returns -EIO when stopping is impossible (signal blocked/
 * ignored or an orphaned pgrp), 1 when the caller stopped and was continued
 * (retry the I/O), else 0 to proceed. `sig` is SIGTTIN or SIGTTOU. */
int tty_jobctl_check(void *tty, vfs_fops_t *tty_fops, pid_t fg_pgrp, int sig)
{
	process_t *p = task_current();
	if (!p || p->ctty != tty || p->ctty_fops != tty_fops) return 0;
	if (fg_pgrp <= 0 || p->pgrp == fg_pgrp) return 0;

	uint64_t bit = 1UL << (sig - 1);
	void (*h)(int) = p->sighand->actions[sig - 1].sa_handler;
	int ignored = ((uintptr_t)h == 1);
	int blocked = (p->sig_mask & bit) != 0;
	if (ignored || blocked) return -5;   /* EIO */

	uint64_t f = spin_lock_irq(&sched_lock);
	int orphan = pgrp_is_orphaned_locked(p->pgrp, p->sid);
	spin_unlock_irq(&sched_lock, f);
	if (orphan) return -5;   /* EIO */

	/* Stop the whole pgrp; the others stop on their own user-return dispatch.
	 * The current task stops here, inside the syscall, then retries the I/O when
	 * SIGCONT resumes it (Linux's tty_check_change). */
	task_signal_pgrp(p->pgrp, sig);
	job_stop_self(sig);
	return 1;
}

void tty_hangup(void *tty, pid_t fg_pgrp)
{
	if (fg_pgrp > 0) {
		task_signal_pgrp(fg_pgrp, SIGHUP);
		task_signal_pgrp(fg_pgrp, SIGCONT);
	}
	uint64_t f = spin_lock_irq(&sched_lock);
	process_t *sentinel = proctab[0];
	for (process_t *t = sentinel->next; t && t != sentinel; t = t->next) {
		if (t->ctty == tty) { t->ctty = NULL; t->ctty_fops = NULL; }
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
		if (sig == SIGCONT) return 1;   /* always resumes a stopped task */
		if ((uintptr_t)handler == 1) continue;   /* SIG_IGN */
		if (!handler && sig_default_stop(sig)) return 1;
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

	/* Serialize with the lock-holding signal posters (task_signal*, and
	 * sched_tick's SIGALRM/SIGVTALRM/SIGPROF). On aarch64 a timer tick can nest
	 * into this IRQ-on syscall-return path; without the lock this read-modify-
	 * write could erase a bit a poster OR'd in concurrently — a silently dropped
	 * signal. (Cross-CPU on both arches.) */
	sigqueue_t si;
	int have_si = 0;
	uint64_t sigf = spin_lock_irq(&sched_lock);
	if (sig >= SIGRTMIN && sig <= SIGRTMAX) {
		have_si = sigq_pop_locked(p, sig, &si);
		/* Keep the pending bit set if more instances of this RT signal remain
		 * queued, so the next delivery picks them up; otherwise clear it. */
		if (!sigq_has_locked(p, sig))
			p->sig_pending &= ~(1UL << (sig - 1));
	} else {
		p->sig_pending &= ~(1UL << (sig - 1));
	}
	spin_unlock_irq(&sched_lock, sigf);

	if (!have_si) {
		si.si_signo = sig;
		si.si_errno = 0;
		si.si_code  = 0;   /* SI_USER */
		si.si_pid   = 0;
		si.si_uid   = 0;
		si.si_value = 0;
	}

	sigaction_t *sa = &p->sighand->actions[sig - 1];
	void (*handler)(int) = sa->sa_handler;

	/* Job-control stop: SIG_DFL for SIGSTOP/TSTP/TTIN/TTOU stops the process.
	 * SIGSTOP is uncatchable. We sleep here on the user-return path; SIGCONT (or
	 * a fatal signal) resumes us via sig_cont_stop_locked clearing job_stopped. */
	if (sig == SIGSTOP || (sig_default_stop(sig) && !handler)) {
		job_stop_self(sig);
		return;
	}

	/* SIGCONT: default action is continue+ignore; the resume already happened at
	 * post time. Run a user handler if one is installed, else just return. */
	if (sig == SIGCONT && (!handler || (uintptr_t)handler == 1)) {
		process_t *par = proc_find(p->ppid);
		if (par) task_wake(par);
		return;
	}

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

	int siginfo_wanted = (sa->sa_flags & 4) != 0;   /* SA_SIGINFO */

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
	 *   [sp+720]  padding × 2
	 *   [sp+736]  siginfo_t (128 bytes, SA_SIGINFO)  (→ total 864)
	 *
	 * The FP/SIMD state MUST round-trip: musl's memcpy/strlen are vectorised,
	 * so a signal landing mid-copy whose handler clobbers q-regs would resume
	 * the interrupted op with garbage SIMD state and corrupt memory silently.
	 */
	uint64_t usp = (tf->sp - 864) & ~15UL;
	/* If the user stack can't hold the frame (bad/overflowed sp), writing it
	 * would fault in EL1 and panic. Linux force_sigsegv's the task instead. */
	if (!access_ok((void *)usp, 864) || !user_pages_mapped((void *)usp, 864)) {
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
	if (siginfo_wanted) {
		uint8_t *info = (uint8_t *)(usp + 736);
		memset(info, 0, 128);
		*(int32_t  *)(info +  0) = si.si_signo;
		*(int32_t  *)(info +  4) = si.si_errno;
		*(int32_t  *)(info +  8) = si.si_code;
		*(int32_t  *)(info + 16) = (int32_t)si.si_pid;
		*(uint32_t *)(info + 20) = (uint32_t)si.si_uid;
		*(uint64_t *)(info + 24) = si.si_value;
	}
	tf->sp                   = usp;
	tf->regs[30]             = sa->sa_restorer ? (uint64_t)sa->sa_restorer
	                                            : tf->elr;
	tf->regs[0]              = (uint64_t)sig;
	if (siginfo_wanted) {
		tf->regs[1] = usp + 736;   /* siginfo_t * */
		tf->regs[2] = 0;           /* ucontext * (unused by RT sigqueue clients) */
	}
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
	 *   [rsp+168]  fxsave area (512 bytes, 16-byte aligned)
	 *   [rsp+680]  siginfo_t (128 bytes, SA_SIGINFO)  (→ total 808)
	 * rt_sigreturn reads base = ctx->rsp (= frame_base+8 after handler ret).
	 * The FP/SSE state must round-trip or a handler's xmm use corrupts the
	 * interrupted code's vector registers (same class as the aarch64 bug). */
	uint64_t usp = (tf->rsp & ~15UL) - 808; /* 8-mod-16 entry; usp+168 is 16-aligned */
	/* Bad/overflowed user sp: writing the frame would fault in the kernel and
	 * panic. Force-kill with SIGSEGV instead. */
	if (!access_ok((void *)usp, 808) || !user_pages_mapped((void *)usp, 808)) {
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
	if (siginfo_wanted) {
		uint8_t *info = (uint8_t *)(usp + 680);
		memset(info, 0, 128);
		*(int32_t  *)(info +  0) = si.si_signo;
		*(int32_t  *)(info +  4) = si.si_errno;
		*(int32_t  *)(info +  8) = si.si_code;
		*(int32_t  *)(info + 16) = (int32_t)si.si_pid;
		*(uint32_t *)(info + 20) = (uint32_t)si.si_uid;
		*(uint64_t *)(info + 24) = si.si_value;
	}
	tf->rsp                  = usp;
	tf->rdi                  = (uint64_t)sig;
	if (siginfo_wanted) {
		tf->rsi = usp + 680;   /* siginfo_t * */
		tf->rdx = 0;           /* ucontext * */
	}
	tf->rip                  = (uint64_t)handler;
#endif
}
