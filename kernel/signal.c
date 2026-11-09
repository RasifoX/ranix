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
	if (sig == SIGKILL && !p->kill_posted_at) p->kill_posted_at = kernel_ticks ? kernel_ticks : 1;
	sig_cont_stop_locked(p, sig);
	sig_wake_locked(p);
	spin_unlock_irq(&sched_lock, f);
}

int fatal_signal_pending(process_t *p)
{
	if (!p) return 0;
	if (p->sig_pending & (1UL << (SIGKILL - 1))) return 1;
	return p->group_exiting || p->exiting;
}

int kloop_should_stop(void)
{
	process_t *p = task_current();
	if (!p || !p->pid) return 0;          /* idle / early boot: nothing to kill */
	cond_resched();
	return fatal_signal_pending(p);
}

int task_signal_info(process_t *p, int sig, int si_code, pid_t si_pid,
                     uid_t si_uid, uint64_t si_value, int thread_directed)
{
	if (!p || sig <= 0 || sig > NSIG) return -EINVAL;

	/* Standard signals coalesce — the bitmask is sufficient and the siginfo is
	 * synthesized on delivery. Only RT signals queue. */
	int is_rt = (sig >= SIGRTMIN && sig <= SIGRTMAX);

	sigqueue_t *q = NULL;
	if (is_rt) {
		q = kmalloc(sizeof(*q));
		if (!q) return -EAGAIN;
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
			return -EAGAIN;
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

/* exit_group and fatal signals end every thread, not just the caller. A survivor
 * would pin the shared fd table, and with it the write end of any stdout pipe, so
 * a reader never sees EOF. Each thread gets the group's status now; SIGKILL only
 * gets it out of whatever it is blocked in. */
void task_group_exit(process_t *p, int code, int by_sig)
{
	uint64_t f = spin_lock_irq(&sched_lock);
	process_t *head = proctab[0];
	for (process_t *t = head->next; t && t != head; t = t->next) {
		if (t == p || t->tgid != p->tgid) continue;
		t->exit_code     = code;      /* also a zombie leader awaiting wait() */
		t->killed_by_sig = by_sig;
		if (t->state == PROC_DEAD || t->state == PROC_ZOMBIE) continue;
		t->group_exiting = 1;
		t->sig_pending  |= 1UL << (SIGKILL - 1);
		sig_wake_locked(t);
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

/* Does group pgrp have a live member in session sid? */
int task_pgrp_in_session(pid_t pgrp, pid_t sid)
{
	uint64_t f = spin_lock_irq(&sched_lock);
	process_t *sentinel = proctab[0];
	int found = 0;
	for (process_t *t = sentinel->next; t && t != sentinel; t = t->next) {
		if (t->pgrp == pgrp && t->sid == sid && t->state != PROC_DEAD && t->state != PROC_ZOMBIE) { found = 1; break; }
	}
	spin_unlock_irq(&sched_lock, f);
	return found;
}

int task_in_group(process_t *p, gid_t g)
{
	if (p->egid == g) return 1;
	for (int i = 0; i < p->ngroups && i < 32; i++)
		if (p->groups[i] == g) return 1;
	return 0;
}

/* POSIX kill permission. CAP_KILL counts only from the initial user namespace,
 * and SIGCONT also passes within a session so a shell can resume a setuid job. */
int signal_may_send(process_t *s, process_t *t, int sig)
{
	if (s->asess && s->asess != t->asess) return 0;
	if (s->tgid == t->tgid) return 1;
	if (s->euid == t->uid || s->euid == t->suid || s->uid == t->uid || s->uid == t->suid)
		return 1;
	if (capable_task(s, CAP_KILL)) return 1;
	return sig == SIGCONT && s->sid == t->sid;
}

/* kill(2) to process group pgrp, or with pgrp == 0 to every process except the
 * sender's own and its pid namespace's init. Only tasks the sender's namespace
 * can see count, even for a group that spans namespaces. sig 0 only probes.
 * Returns 0, -ESRCH (no target) or -EPERM (no target it may signal). */
int task_kill_many(process_t *s, pid_t pgrp, int sig)
{
	pid_namespace_t *ns = current_pid_ns();
	int seen = 0, sent = 0;
	uint64_t f = spin_lock_irq(&sched_lock);
	process_t *sentinel = proctab[0];
	for (process_t *t = sentinel->next; t && t != sentinel; t = t->next) {
		if (t->state == PROC_DEAD || t->state == PROC_ZOMBIE) continue;
		int nr = task_pid_nr_ns(t, ns);
		if (!nr) continue;
		if (pgrp ? t->pgrp != pgrp : (nr == 1 || t->tgid == s->tgid)) continue;
		seen++;
		if (!signal_may_send(s, t, sig)) continue;
		sent++;
		if (sig) {
			t->sig_pending |= (1UL << (sig - 1));
			sig_cont_stop_locked(t, sig);
			sig_wake_locked(t);
		}
	}
	spin_unlock_irq(&sched_lock, f);
	return sent ? 0 : seen ? -1 : -3;
}

/* SIGKILL every member of an agent session. */
void task_kill_session(struct asess *s)
{
	uint64_t f = spin_lock_irq(&sched_lock);
	process_t *sentinel = proctab[0];
	for (process_t *t = sentinel->next; t && t != sentinel; t = t->next) {
		if (t->asess != s) continue;
		t->sig_pending |= 1UL << (SIGKILL - 1);
		sig_wake_locked(t);
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
	proc_wake_waiters(p->ppid);
	arch_get_cpu_ctx()->wd_where = "sig:stop";
	while (p->job_stopped && !(p->sig_pending & (1UL << (SIGKILL - 1))))
		task_sleep_if(p->job_stopped && !(p->sig_pending & (1UL << (SIGKILL - 1))));
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
	if (ignored || blocked) return -EIO;

	uint64_t f = spin_lock_irq(&sched_lock);
	int orphan = pgrp_is_orphaned_locked(p->pgrp, p->sid);
	spin_unlock_irq(&sched_lock, f);
	if (orphan) return -EIO;

	/* Stop the whole pgrp; the others stop on their own user-return dispatch.
	 * The current task stops here, inside the syscall, then retries the I/O when
	 * SIGCONT resumes it (Linux's tty_check_change). */
	task_signal_pgrp(p->pgrp, sig);
	job_stop_self(sig);
	return 1;
}

/* The session holding tty, read from its members' ctty pointers: unlike a field
 * on the tty, this cannot outlive the session. */
static int tty_held_elsewhere_locked(void *tty, pid_t sid)
{
	process_t *head = proctab[0];
	for (process_t *t = head->next; t && t != head; t = t->next)
		if (t->ctty == tty && t->sid != sid &&
		    t->state != PROC_ZOMBIE && t->state != PROC_DEAD) return 1;
	return 0;
}

/* TIOCSCTTY: a session leader without a terminal takes one nobody holds. With
 * steal set and CAP_SYS_ADMIN it may take one from another session. Returns 1
 * when the caller already holds tty, 0 when it just took it. */
int tty_acquire(void *tty, vfs_fops_t *fops, int steal)
{
	process_t *p = task_current();
	if (p->ctty == tty) return 1;
	if (p->ctty || p->sid != (pid_t)p->pid) return -EPERM;
	uint64_t f = spin_lock_irq(&sched_lock);
	if (tty_held_elsewhere_locked(tty, p->sid)) {
		if (!steal || !capable_task(p, CAP_SYS_ADMIN)) {
			spin_unlock_irq(&sched_lock, f);
			return -1;
		}
		process_t *head = proctab[0];
		for (process_t *t = head->next; t && t != head; t = t->next)
			if (t->ctty == tty) { t->ctty = NULL; t->ctty_fops = NULL; }
	}
	p->ctty = tty;
	p->ctty_fops = fops;
	spin_unlock_irq(&sched_lock, f);
	return 0;
}

/* TIOCSPGRP: the new foreground group must be a group of the caller's session.
 * The terminal must be the caller's own, except on the console (need_ctty 0):
 * init starts its shell there with no controlling terminal. */
long tty_check_fg(void *tty, int need_ctty, pid_t nr, pid_t *out)
{
	process_t *p = task_current();
	if (nr < 0) return -EINVAL;
	if (need_ctty ? p->ctty != tty : p->ctty && p->ctty != tty) return -ENOTTY;
	pid_t pgrp = pid_from_user(nr);
	if (!pgrp) return -ESRCH;
	long r = -3;
	uint64_t f = spin_lock_irq(&sched_lock);
	process_t *head = proctab[0];
	for (process_t *t = head->next; t && t != head; t = t->next) {
		if (t->pgrp != pgrp) continue;
		r = t->sid == p->sid ? 0 : -1; /* EPERM */
		break;
	}
	spin_unlock_irq(&sched_lock, f);
	if (r == 0) *out = pgrp;
	return r;
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
	case 4:  /* SIGILL  */
	case 5:  /* SIGTRAP */
	case 6:  /* SIGABRT */
	case 7:  /* SIGBUS  */
	case 8:  /* SIGFPE  */
	case 9:  /* SIGKILL */
	case 11: /* SIGSEGV */
	case 13: /* SIGPIPE */
	case 14: /* SIGALRM */
	case 10: /* SIGUSR1 */
	case 12: /* SIGUSR2 */
	case 15: /* SIGTERM */
	case 16: /* SIGSTKFLT */
	case 24: /* SIGXCPU */
	case 25: /* SIGXFSZ */
	case 26: /* SIGVTALRM */
	case 27: /* SIGPROF */
	case 29: /* SIGIO   */
	case 30: /* SIGPWR  */
	case 31: /* SIGSYS  */
		return 1;
	default:
		return sig >= SIGRTMIN;
	}
}

#define SA_NODEFER   0x40000000UL
#define SA_RESTART   0x10000000UL

/* Back up over the syscall instruction, so the task enters the call again on its
 * way out of the handler. The argument registers are still as the call found them. */
static void syscall_rewind(arch_context_t *tf, process_t *p)
{
#if defined(__x86_64__)
	tf->rip -= 2;
	tf->rax  = (uint64_t)(long)p->last_syscall;
#elif defined(__aarch64__)
	tf->elr -= 4;
	tf->regs[0] = p->sys_arg0;
#endif
}
#define SA_ONSTACK   0x08000000UL

/* Where this delivery's frame goes: below sp, or at the top of the alternate stack
 * for an SA_ONSTACK handler unless already running on it. QEMU's coroutines and any
 * handler for a stack overflow depend on the frame not landing on the old stack. */
static uint64_t sig_frame_sp(process_t *p, unsigned long sa_flags, uint64_t sp)
{
	uint64_t lo = (uint64_t)p->altstack.ss_sp, size = p->altstack.ss_size;
	if (!(sa_flags & SA_ONSTACK) || size == 0 || (p->altstack.ss_flags & SS_DISABLE)) return sp;
	if (sp >= lo && sp - lo < size) return sp;
	return lo + size;
}
#define SA_RESETHAND 0x80000000UL

int task_has_actionable_signal(void)
{
	process_t *p = task_current();
	if (!p->sighand) return 1;           /* exiting: a driver's close must not wait on */
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

static void signal_dispatch_one(arch_context_t *tf);

/* Take the lowest signal of `set` pending on p, or left on its thread group when
 * every thread blocked it. Returns it, 0 if none; *have says si was filled in. */
int task_dequeue_signal(process_t *p, uint64_t set, sigqueue_t *si, int *have)
{
	int sig = 0;
	*have = 0;
	uint64_t f = spin_lock_irq(&sched_lock);
	process_t *t = p;
	if (!(p->sig_pending & set))
		for (process_t *c = proctab[0]->next; c && c != proctab[0]; c = c->next)
			if (c->tgid == p->tgid && (c->sig_pending & set)) { t = c; break; }
	uint64_t ready = t->sig_pending & set;
	if (ready) {
		sig = __builtin_ctzl(ready) + 1;
		if (sig >= SIGRTMIN && sig <= SIGRTMAX) {
			*have = sigq_pop_locked(t, sig, si);
			if (!sigq_has_locked(t, sig)) t->sig_pending &= ~(1UL << (sig - 1));
		} else {
			t->sig_pending &= ~(1UL << (sig - 1));
		}
	}
	spin_unlock_irq(&sched_lock, f);
	return sig;
}

/* The task's own instruction faulted. It cannot go on past the fault, so unlike a
 * posted signal this one is not optional: blocked or ignored, it takes the default
 * action. A handler runs as usual, and may fix the cause or longjmp away. */
/* A synchronous fault's siginfo: si_addr shares si_pid and si_uid */
static void fault_siginfo(process_t *p, sigqueue_t *si)
{
	si->si_code = p->fault_code;
	si->si_pid  = (pid_t)(uint32_t)p->fault_addr;
	si->si_uid  = (uid_t)(p->fault_addr >> 32);
	si->si_errno = p->fault_errno;
	si->si_value = p->fault_value;   /* SIGSYS: _syscall, _arch */
}

/* Blocked or ignored, a forced signal takes its default action. Caller holds
 * sched_lock. */
static void force_unblock(process_t *p, int sig)
{
	uint64_t bit = 1UL << (sig - 1);
	sigaction_t *sa = &p->sighand->actions[sig - 1];
	if ((p->sig_mask & bit) || (uintptr_t)sa->sa_handler == 1) {
		sa->sa_handler = 0;
		p->sig_mask &= ~bit;
	}
}

/* SECCOMP_RET_TRAP (Linux force_sig_seccomp): SIGSYS on the way out of the
 * syscall the filter did not let run. si_code SYS_SECCOMP, si_call_addr just
 * past the syscall instruction, si_syscall and si_arch, si_errno the filter's
 * data. Forced: blocked or ignored, it kills. */
void task_force_seccomp(uint64_t call_addr, int nr, uint32_t arch, int data)
{
	process_t *p = task_current();
	int sig = 31;   /* SIGSYS */
	uint64_t f = spin_lock_irq(&sched_lock);
	force_unblock(p, sig);
	p->fault_sig   = sig;
	p->fault_code  = 1;   /* SYS_SECCOMP */
	p->fault_addr  = call_addr;
	p->fault_err   = 0;
	p->fault_trap  = 0;
	p->fault_value = (uint32_t)nr | ((uint64_t)arch << 32);
	p->fault_errno = data;
	p->sig_pending |= 1UL << (sig - 1);
	spin_unlock_irq(&sched_lock, f);
}

void task_force_fault(arch_context_t *tf, int sig, int code, uint64_t addr, uint64_t err, uint64_t trap)
{
	process_t *p = task_current();
	uint64_t bit = 1UL << (sig - 1);
	uint64_t f = spin_lock_irq(&sched_lock);
	force_unblock(p, sig);
	p->fault_value = 0;
	p->fault_errno = 0;
	p->fault_sig  = sig;
	p->fault_code = code;
	p->fault_addr = addr;
	p->fault_err  = err;
	p->fault_trap = trap;
	p->sig_pending |= bit;
	spin_unlock_irq(&sched_lock, f);
	signal_dispatch(tf);
}

/* Would this fault end the process? Then its diagnostics are worth logging. */
int task_fault_fatal(int sig)
{
	process_t *p = task_current();
	void (*h)(int) = p->sighand->actions[sig - 1].sa_handler;
	return (uintptr_t)h <= 1 || (p->sig_mask & (1UL << (sig - 1)));
}

void signal_dispatch(arch_context_t *tf)
{
	signal_dispatch_one(tf);
	process_t *p = task_current();
	/* interrupted by a signal that ran no handler: nobody should see the EINTR */
	if (p && p->sys_intr) {
		p->sys_intr = 0;
		syscall_rewind(tf, p);
	}
#if defined(__x86_64__)
	/* iretq to a non-canonical rip or rsp faults in ring 0, on an address the
	 * user chose through sigaction, sigreturn, ptrace or clone. */
	if (p && (tf->cs & 3) == 3 && ((tf->rip | tf->rsp) >> 47)) {
		p->exit_code = 11; p->killed_by_sig = 1;
		arch_get_cpu_ctx()->wd_where = "sig:badframe";
		task_exit();
	}
#endif
	/* also runs on kernel frames, where a sleeping pselect still needs its mask */
	if (p && p->sig_mask_saved && !p->in_syscall) {   /* nothing was delivered under it */
		p->sig_mask = p->sig_saved_mask;
		p->sig_mask_saved = 0;
	}
}

/* The frame could not be written to the user stack: the task dies of SIGSEGV
 * (Linux force_sigsegv), it does not fault in the kernel. */
static void sig_kill_badframe(process_t *p)
{
	p->exit_code = 11; p->killed_by_sig = 1;
	arch_get_cpu_ctx()->wd_where = "sig:badframe";
	task_exit();
}

/* siginfo_t's first 32 bytes, the rest zero: signo, errno, code, pid, uid, value. */
static void sig_put_siginfo(uint8_t *info, const sigqueue_t *si)
{
	memset(info, 0, 128);
	*(int32_t  *)(info +  0) = si->si_signo;
	*(int32_t  *)(info +  4) = si->si_errno;
	*(int32_t  *)(info +  8) = si->si_code;
	*(int32_t  *)(info + 16) = (int32_t)si->si_pid;
	*(uint32_t *)(info + 20) = (uint32_t)si->si_uid;
	*(uint64_t *)(info + 24) = si->si_value;
}

/* Take the lowest unblocked pending signal off p and fill *si for it.
 * Returns it, or 0 when nothing is deliverable. */
static int sig_take_next(process_t *p, sigqueue_t *si)
{
	uint64_t mask = p->sig_pending & ~p->sig_mask;
	if (!mask) return 0;

	int sig = 0;
	for (int i = 0; i < NSIG; i++) {
		if (mask & (1UL << i)) { sig = i + 1; break; }
	}
	if (!sig) return 0;

	/* Serialize with the lock-holding signal posters (task_signal*, and
	 * sched_tick's SIGALRM/SIGVTALRM/SIGPROF). On aarch64 a timer tick can nest
	 * into this IRQ-on syscall-return path; without the lock this read-modify-
	 * write could erase a bit a poster OR'd in concurrently — a silently dropped
	 * signal. (Cross-CPU on both arches.) */
	int have_si = 0;
	uint64_t sigf = spin_lock_irq(&sched_lock);
	if (sig >= SIGRTMIN && sig <= SIGRTMAX) {
		have_si = sigq_pop_locked(p, sig, si);
		/* Keep the pending bit set if more instances of this RT signal remain
		 * queued, so the next delivery picks them up; otherwise clear it. */
		if (!sigq_has_locked(p, sig))
			p->sig_pending &= ~(1UL << (sig - 1));
	} else {
		p->sig_pending &= ~(1UL << (sig - 1));
	}
	spin_unlock_irq(&sched_lock, sigf);

	if (!have_si) {
		si->si_signo = sig;
		si->si_errno = 0;
		si->si_code  = 0;   /* SI_USER */
		si->si_pid   = 0;
		si->si_uid   = 0;
		si->si_value = 0;
		if (p->fault_sig == sig) fault_siginfo(p, si);
	}
	return sig;
}

/* Everything but running a user handler: stop, continue, ignore or the default
 * action. 1: the signal was dealt with here. */
static int sig_no_handler(arch_context_t *tf, process_t *p, int sig, void (*handler)(int))
{

	/* Job-control stop: SIG_DFL for SIGSTOP/TSTP/TTIN/TTOU stops the process.
	 * SIGSTOP is uncatchable. We sleep here on the user-return path; SIGCONT (or
	 * a fatal signal) resumes us via sig_cont_stop_locked clearing job_stopped.
	 *
	 * If the task is traced, a stopping signal is a ptrace group-stop: report it
	 * to the tracer (unconditionally, not gated on waitpid WUNTRACED) and sleep
	 * until PTRACE_CONT/DETACH or a fatal signal. Without this an ATTACH-induced
	 * SIGSTOP on a sleeping tracee stops the task but the tracer's plain waitpid
	 * never sees it — a permanent wedge. */
	if (sig == SIGSTOP || (sig_default_stop(sig) && !handler)) {
		if (p->tracer) {
			ptrace_group_stop(sig);
		} else {
			job_stop_self(sig);
		}
		/* Sleeping here saved a deeper frame in p->ctx, and the return path
		 * restores from p->ctx: point it back, or it resumes from a dead frame. */
		p->ctx = tf;
		return 1;
	}

	/* SIGCONT: default action is continue+ignore; the resume already happened at
	 * post time. Run a user handler if one is installed, else just return. */
	if (sig == SIGCONT && (!handler || (uintptr_t)handler == 1)) {
		process_t *par = proc_find(p->ppid);
		if (par) task_wake(par);
		proc_wake_waiters(p->ppid);
		return 1;
	}

	/* SIG_IGN = 1 */
	if ((uintptr_t)handler == 1) return 1;

	/* SIG_DFL = 0 or NULL */
	if (!handler) {
		if (sig_default_kill(sig)) {
			p->exit_code      = sig;
			p->killed_by_sig  = 1;
			coredump(p, sig, tf);
			arch_get_cpu_ctx()->wd_where = "sig:kill";
			task_exit();
		}
		return 1;
	}
	return 0;
}

#if defined(__aarch64__)
/* Build the handler frame on the user stack and point tf at the handler. */
static void sig_setup_frame(arch_context_t *tf, process_t *p, int sig, void (*handler)(int),
                            const sigaction_t *sa, const sigqueue_t *si, uint64_t old_mask)
{
	int siginfo_wanted = (sa->sa_flags & 4) != 0;   /* SA_SIGINFO */
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
	uint64_t usp = (sig_frame_sp(p, sa->sa_flags, tf->sp) - 864) & ~15UL;
	/* If the user stack can't hold the frame (bad/overflowed sp), writing it
	 * would fault in EL1 and panic. Linux force_sigsegv's the task instead. */
	if (!access_ok((void *)usp, 864)) {   /* a lazy page is fine: copy_to_user below faults it in */
		sig_kill_badframe(p);
	}
	/* Staged in the kernel: a frame on a read-only stack passes the mapped check
	 * above and would fault on a raw store. */
	uint8_t kframe[864] __attribute__((aligned(16)));
	uint64_t uframe = usp;
	usp = (uint64_t)kframe;
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
	if (siginfo_wanted) sig_put_siginfo((uint8_t *)(usp + 736), si);
	usp = uframe;
	if (copy_to_user((void *)usp, kframe, sizeof(kframe)) != 0) {
		sig_kill_badframe(p);
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
}
#elif defined(__x86_64__)
/* ucontext_t: flags, link, stack (SS_DISABLE), mcontext gregs[23] in
 * REG_R8..REG_CR2 order, fpregs → the fxsave area, sigmask */
static void sig_fill_ucontext(arch_context_t *tf, process_t *p, int sig, uint8_t *uc,
                              uint64_t uframe, uint64_t old_mask)
{
	memset(uc, 0, 432);
	*(int32_t  *)(uc + 24) = 2;                      /* uc_stack.ss_flags = SS_DISABLE */
	uint64_t *g = (uint64_t *)(uc + 40);
	g[0]  = tf->r8;  g[1]  = tf->r9;  g[2]  = tf->r10; g[3]  = tf->r11;
	g[4]  = tf->r12; g[5]  = tf->r13; g[6]  = tf->r14; g[7]  = tf->r15;
	g[8]  = tf->rdi; g[9]  = tf->rsi; g[10] = tf->rbp; g[11] = tf->rbx;
	g[12] = tf->rdx; g[13] = tf->rax; g[14] = tf->rcx; g[15] = tf->rsp;
	g[16] = tf->rip; g[17] = tf->rflags;
	g[18] = 0x33;                                    /* csgsfs: user cs */
	if (p->fault_sig == sig) {
		g[19] = p->fault_err;  g[20] = p->fault_trap;  g[22] = p->fault_addr;
		p->fault_sig = 0;
	}
	g[21] = old_mask;
	*(uint64_t *)(uc + 224) = uframe + 168;          /* fpregs */
	*(uint64_t *)(uc + 296) = old_mask;              /* uc_sigmask */
}

/* Build the handler frame on the user stack and point tf at the handler. */
static void sig_setup_frame(arch_context_t *tf, process_t *p, int sig, void (*handler)(int),
                            const sigaction_t *sa, const sigqueue_t *si, uint64_t old_mask)
{
	int siginfo_wanted = (sa->sa_flags & 4) != 0;   /* SA_SIGINFO */
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
	 *   [rsp+680]  siginfo_t (128 bytes, SA_SIGINFO)
	 *   [rsp+808]  ucontext_t, Linux x86_64 layout (424 bytes) + 8 pad
	 *   [rsp+1240] upper halves of ymm0-15 (256 bytes, → total 1496)
	 * rt_sigreturn reads base = ctx->rsp (= frame_base+8 after handler ret).
	 * SA_SIGINFO handlers get the ucontext in rdx: JavaScriptCore's GC suspend
	 * handler reads uc_mcontext.gregs[REG_RSP] and faulted at 0xa0 when it was
	 * NULL. It is a snapshot: rt_sigreturn restores from the fields above, so
	 * edits a handler makes to the ucontext are not applied.
	 * The FP/SSE state must round-trip or a handler's xmm use corrupts the
	 * interrupted code's vector registers (same class as the aarch64 bug). */
	/* 128: the System V red zone, which a handler on the same stack must not touch.
	 * 8-mod-16 entry; usp+168 is 16-aligned. */
	uint64_t usp = ((sig_frame_sp(p, sa->sa_flags, tf->rsp - 128) & ~15UL) - 1496);
	/* Bad/overflowed user sp: writing the frame would fault in the kernel and
	 * panic. Force-kill with SIGSEGV instead. */
	if (!access_ok((void *)usp, 1496)) {   /* a lazy page is fine: copy_to_user below faults it in */
		sig_kill_badframe(p);
	}
	/* Staged in the kernel: a frame on a read-only stack passes the mapped check
	 * above and would fault on a raw store. */
	uint8_t kframe[1496] __attribute__((aligned(16)));
	uint64_t uframe = usp;
	usp = (uint64_t)kframe;
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
	memcpy((void *)(usp + 1240), tf->ymmh, sizeof(tf->ymmh));
	if (siginfo_wanted) sig_put_siginfo((uint8_t *)(usp + 680), si);
	sig_fill_ucontext(tf, p, sig, (uint8_t *)(usp + 808), uframe, old_mask);
	usp = uframe;
	if (copy_to_user((void *)usp, kframe, sizeof(kframe)) != 0) {
		sig_kill_badframe(p);
	}
	tf->rsp                  = usp;
	tf->rdi                  = (uint64_t)sig;
	if (siginfo_wanted) {
		tf->rsi = usp + 680;   /* siginfo_t * */
		tf->rdx = usp + 808;   /* ucontext_t * */
	}
	tf->rip                  = (uint64_t)handler;
}
#endif

static void signal_dispatch_one(arch_context_t *tf)
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
	kstack_check();

	/* (Re)arm single-step TF on this user return, independent of signals. */
	ptrace_apply_singlestep(tf);

	if (p->group_exiting) task_exit();

	sigqueue_t si;
	int sig = sig_take_next(p, &si);
	if (!sig) return;

	/* A traced task shows the tracer each signal first; stops have their own path */
	if (p->tracer && sig != SIGKILL && !(sig == SIGSTOP || sig_default_stop(sig))) {
		int s2 = ptrace_signal_stop(tf, sig, &si);   /* fills si for s2 */
		if (!s2) return;              /* the tracer dropped it */
		sig = s2;
	}

	sigaction_t *sa = &p->sighand->actions[sig - 1];
	void (*handler)(int) = sa->sa_handler;

	if (sig_no_handler(tf, p, sig, handler)) return;
	arch_get_cpu_ctx()->wd_where = "sig:frame";

	if (p->sys_intr) {
		p->sys_intr = 0;
		if (sa->sa_flags & SA_RESTART) syscall_rewind(tf, p);
	}

	/* Deliver to user handler: redirect the trap frame */
	/* Block this signal during handler execution; save old mask for restore */
	uint64_t old_mask = p->sig_mask_saved ? p->sig_saved_mask : p->sig_mask;
	p->sig_mask_saved = 0;
	if (!(sa->sa_flags & SA_NODEFER)) p->sig_mask |= (1UL << (sig - 1));
	p->sig_mask |= sa->sa_mask;

	/* Node's SIGTERM handler resets stdio and re-raises: without the reset it runs
	 * again for ever, and no SIGTERM ends the process */
	if (sa->sa_flags & SA_RESETHAND) sa->sa_handler = NULL;

	sig_setup_frame(tf, p, sig, handler, sa, &si, old_mask);
}
