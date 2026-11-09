/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* ── hung-task detector (khungtaskd parity) ──────────────────────────────────
 * A always-on, cheap net for the failure that today just hangs in silence: a
 * task the scheduler believes will make progress that never does — a lost
 * wakeup, a missed reschedule IPI, an orphaned runqueue entry, a deadlock on an
 * uninterruptible kernel block. The existing sched_watchdog catches a *CPU*
 * wedged with IRQs off (wd_ticks frozen); this catches a *task* that is stuck
 * while its CPU keeps taking ticks.
 *
 * Progress is p->ticks (bumped when a tick lands on the task) plus its dispatch
 * count, the equivalent of Linux's last_switch_count. A watched task
 * whose ticks stay frozen for longer than the timeout while kernel_ticks marches
 * on is reported LOUDLY — pid, comm, state, the last syscall it entered, and a
 * frame-pointer backtrace walked from its saved context.
 *
 * Zero false positives on a healthy kernel is the hard requirement, so "watched"
 * is deliberately narrow:
 *   - PROC_READY (runnable): a queued task MUST be dispatched within a couple of
 *     quanta. 20s of "ready but never ran" is unambiguously a scheduler bug.
 *   - hung_watch: an *uninterruptible* kernel block that owes a producer wakeup.
 * Plain PROC_SLEEPING is NOT watched: in Ranix every sleeper is woken by a signal
 * (interruptible, Linux's TASK_INTERRUPTIBLE), and pause(2)/blocking recv/accept/
 * waitpid all sit there indefinitely by design — exactly the workload khungtaskd
 * must never flag. Only blocks that opt in via hung_watch are timed out.
 * ─────────────────────────────────────────────────────────────────────────── */

#include <kernel/proc.h>
#include <kernel/hungtask.h>
#include <kernel/hal.h>
#include <kernel/sync.h>
#include <kernel/cmdline.h>
#include <kernel/ksyms.h>
#include <kernel/kprintf.h>
#include <kernel/board.h>   /* TIMER_HZ */
#include <kernel/bug.h>
#include <kernel/lib/string.h>
#include <kernel/time.h>
#include <kernel/smp.h>

volatile uint64_t hung_task_count;

/* Default 20s. Overridable at boot
 * with hung_task_timeout_secs= (0 disables). Read once, lazily. */
static uint64_t timeout_ticks;
static int      timeout_ready;

static uint64_t hung_timeout_ticks(void)
{
	if (timeout_ready) return timeout_ticks;
	uint64_t secs = 20;
	char buf[16];
	if (cmdline_get("hung_task_timeout_secs", buf, sizeof(buf))) {
		uint64_t v = 0;
		for (const char *s = buf; *s >= '0' && *s <= '9'; s++)
			v = v * 10 + (uint64_t)(*s - '0');
		secs = v;
	}
	timeout_ticks = secs * TIMER_HZ;
	timeout_ready = 1;
	return timeout_ticks;
}

const char *hung_state_name(uint64_t st)
{
	switch (st) {
	case PROC_RUNNING:  return "RUNNING";
	case PROC_READY:    return "READY";
	case PROC_SLEEPING: return "SLEEPING";
	case PROC_ZOMBIE:   return "ZOMBIE";
	case PROC_DEAD:     return "DEAD";
	default:            return "?";
	}
}

/* A task the detector must time out: runnable but not running, or parked on an
 * uninterruptible block. Idle (pid 0) and exiting tasks never qualify. */
static int hung_watched(process_t *p)
{
	if (p->pid == 0) return 0;
	if (p->state == PROC_DEAD || p->state == PROC_ZOMBIE) return 0;
	if (p->state == PROC_READY) return 1;
	return p->hung_watch && p->state == PROC_SLEEPING;
}

/* The decision, factored so the sweep and the self-test run the identical path.
 * Advances p's stall window exactly as a real sweep would and returns the stall
 * duration (ticks) to report, or 0 for "no report". Caller serializes against
 * concurrent state/ticks changes. */
static uint64_t hung_eval(process_t *p, uint64_t now, uint64_t to)
{
	/* A task that wakes and sleeps within one tick (net_rx polling at tick rate)
	 * is never current when the tick lands, so p->ticks alone stays frozen while
	 * the sweep, running in that same tick, sees it READY. Dispatches count too. */
	uint64_t progress = p->ticks + p->hung_switches;
	if (!hung_watched(p) || progress != p->hung_run_ticks) {
		p->hung_run_ticks   = progress;   /* progressed, or not watched */
		p->hung_stall_start = 0;
		return 0;
	}
	if (!p->hung_stall_start) { p->hung_stall_start = now; return 0; }
	if (now - p->hung_stall_start >= to) {
		uint64_t stalled = now - p->hung_stall_start;
		p->hung_stall_start = now;        /* re-arm: warn again after another window */
		return stalled;
	}
	return 0;
}

void hung_backtrace(process_t *p)
{
	arch_context_t *ctx = p->ctx;
	if (!ctx) return;
#if defined(__x86_64__)
	uint64_t pc = ctx->rip, fp = ctx->rbp;
#else
	uint64_t pc = ctx->elr, fp = ctx->regs[29];
#endif
	uint64_t off = 0;
	const char *s = ksym_lookup(pc, &off);
	if (s) kprintf(KERN_WARN "hung:   %s+0x%lx (0x%016lx)\n", s, off, pc);
	else   kprintf(KERN_WARN "hung:   0x%016lx\n", pc);
	for (int i = 0; i < 16 && fp >= 0xFFFF000000000000UL && (fp & 7) == 0; i++) {
		uint64_t next = ((uint64_t *)fp)[0];
		uint64_t ret  = ((uint64_t *)fp)[1];
		if (!ret) break;
		off = 0;
		s = ksym_lookup(ret, &off);
		if (s) kprintf(KERN_WARN "hung:   %s+0x%lx (0x%016lx)\n", s, off, ret);
		else   kprintf(KERN_WARN "hung:   0x%016lx\n", ret);
		if (next <= fp) break;
		fp = next;
	}
}

/* What hung_report prints about one CPU, copied under sched_lock: another CPU
 * changes its queue and current task while we print. cur is pinned (refcount)
 * so it cannot be freed before hung_snap_put. */
struct hung_cpu_snap {
	process_t *cur;
	int        pid, state, sys, rq, rqn, slow, resched, on_cpu;
	uint64_t   wd;
};
static struct hung_cpu_snap hung_snap[MAX_CPUS];   /* hung_task_check runs on one CPU at a time */

/* Under sched_lock: where p is queued and current (-1: nowhere), and, when
 * cpus is set, every CPU's state into hung_snap. Returns the CPUs copied. */
static int hung_snapshot(process_t *p, int *queued_on, int *current_on, int cpus)
{
	int n = 0, where = -1, cur = -1;
	uint64_t f = spin_lock_irq(&sched_lock);
	for (int i = 0; i < smp_ncpus && where < 0; i++) {
		int guard = 0;
		for (process_t *q = all_cpus[i] ? all_cpus[i]->rq_head : NULL; q && guard < 4096; q = q->sched_next, guard++)
			if (q == p) { where = i; break; }
	}
	for (int i = 0; i < smp_ncpus; i++) if (all_cpus[i] && all_cpus[i]->current == p) cur = i;
	for (int i = 0; cpus && i < smp_ncpus && i < MAX_CPUS; i++) {
		cpu_context_t *c = all_cpus[i];
		struct hung_cpu_snap *s = &hung_snap[i];
		memset(s, 0, sizeof(*s));
		s->pid = -1; s->state = -1; s->sys = -1;
		if (!c) continue;
		process_t *q = c->current;
		if (q) {
			s->cur = q; q->refcount++;           /* proc_hold, sched_lock is held */
			s->pid = (int)q->pid; s->state = (int)q->state; s->sys = q->last_syscall;
			s->on_cpu = q->on_cpu;
		}
		for (process_t *r = c->rq_head; r && s->rq < 4096; r = r->sched_next) s->rq++;
		s->rqn = (int)c->rq_n;
		s->slow = c->tick_slow; s->resched = c->need_resched; s->wd = c->wd_ticks;
		n = i + 1;
	}
	spin_unlock_irq(&sched_lock, f);
	*queued_on = where; *current_on = cur;
	return n;
}

/* Drop the pins hung_snapshot took on the first n CPUs' current tasks. */
static void hung_snap_put(int n)
{
	for (int i = 0; i < n; i++) { proc_put(hung_snap[i].cur); hung_snap[i].cur = NULL; }
}

/* Loud, marked, greppable. Distinct from a panic: a hung task is a diagnosis, not
 * a reason to take the machine down — the rest of the system is still running. */
static void hung_report(process_t *p, uint64_t stalled_ticks)
{
	hung_task_count++;
	kprintf(KERN_WARN "hung: task pid=%d (%s) %s for %lu s, last_syscall=%d\n",
	        (int)p->pid, p->name, hung_state_name(p->state),
	        stalled_ticks / TIMER_HZ, p->last_syscall);
	if (p->state == PROC_READY) {     /* runnable and not run: is it on a queue at all? */
		static uint64_t last_dump;
		int dump = kernel_ticks - last_dump > 5 * TIMER_HZ;
		if (dump) last_dump = kernel_ticks;
		int where, cur;
		int n = hung_snapshot(p, &where, &cur, dump);
		kprintf(KERN_WARN "hung: in_rq=%d on_cpu=%d queued_on=%d current_on=%d\n",
		        (int)p->in_rq, (int)p->on_cpu, where, cur);
		for (int i = 0; i < n; i++) {
			struct hung_cpu_snap *s = &hung_snap[i];
			if (!all_cpus[i]) continue;
			kprintf(KERN_WARN "hung: cpu%d current pid=%d (%s) state=%d syscall=%d rq=%d slow=%d resched=%d wd=%lu\n", i,
			        s->pid, s->cur ? s->cur->name : "-", s->state,
			        s->sys, s->rq, s->slow, s->resched, (unsigned long)s->wd);
			/* a running task's saved frame is stale and its stack is live */
			if (s->cur && s->pid) {
				if (s->on_cpu) kprintf(KERN_WARN "hung:   (running on a cpu: no saved frame)\n");
				else hung_backtrace(s->cur);
			}
		}
		hung_snap_put(n);
	}
	kprintf(KERN_WARN "hung: call trace\n");
	hung_backtrace(p);
	kprintf(KERN_WARN "hung: end pid=%d\n", (int)p->pid);
}

/* Once, on the first sweep (early in boot: most CPUs are still in their idle
 * task): the snapshot places a task that is on no queue and runs nowhere at -1,
 * counts each queue as long as its rq_n says (both read under the one lock), and
 * pins what it reports. A CPU's current is either its idle task (pid 0, static
 * or per-CPU PCB from sched_init/secondary_cpu_main, never PROC_MAGIC, never
 * freed) or a PCB from proc_create/proc_exec/clone, which carries PROC_MAGIC
 * until its last pin is dropped. hung_report relies on pid 0 meaning idle. */
static void hung_snapshot_selftest(void)
{
	process_t t;
	memset(&t, 0, sizeof(t));
	int where, cur, bad = 0, ncpu = 0, nidle = 0;
	int n = hung_snapshot(&t, &where, &cur, 1);
	for (int i = 0; i < n; i++) {
		struct hung_cpu_snap *s = &hung_snap[i];
		if (!all_cpus[i]) continue;
		ncpu++;
		if (s->rq < 4096 && s->rq != s->rqn) bad |= 1;
		if (!s->cur) continue;
		if (s->cur->refcount == 0) bad |= 32;                  /* the pin is not held */
		if (s->cur == all_cpus[i]->idle) {
			nidle++;
			if (s->pid != 0) bad |= 16;                        /* idle must read as pid 0 */
		} else {
			if (s->pid == 0) bad |= 16;                        /* pid 0 that is not idle */
			if (s->cur->magic != PROC_MAGIC) bad |= 2;         /* pinned: cannot be freed */
		}
	}
	hung_snap_put(n);
	if (where != -1 || cur != -1) bad |= 4;
	if (n != smp_ncpus && n < MAX_CPUS) bad |= 8;
	if (bad) kprintf(KERN_ERR "hung: snapshot selftest FAILED bad=%d cpus=%d idle=%d\n", bad, ncpu, nidle);
	else     kprintf(KERN_INFO "hung: snapshot selftest ok cpus=%d idle=%d\n", ncpu, nidle);
}

/* One sweep. Gated by the caller to run a few times a minute, so the O(nproc)
 * walk is negligible. Snapshots and advances each task's window under sched_lock
 * (state/ticks change concurrently); prints outside the lock. */
void hung_task_check(void)
{
	uint64_t to = hung_timeout_ticks();
	if (!to) return;   /* disabled */
	static int snap_tested;
	if (!snap_tested) { snap_tested = 1; hung_snapshot_selftest(); }

	uint64_t now = kernel_ticks;
	int n = task_count(), shown = 0, more = 0;
	for (int i = 0; i < n; i++) {
		uint64_t f = spin_lock_irq(&sched_lock);
		process_t *p = task_peek_locked(i);
		uint64_t stalled = (p && p->magic == PROC_MAGIC) ? hung_eval(p, now, to) : 0;
		process_t *victim = stalled ? p : NULL;
		/* a few in full; thousands of them through a serial console would be the
		 * next thing to stall the machine */
		if (victim && shown >= 5) { more++; victim = NULL; }
		if (victim) victim->refcount++;          /* proc_hold, sched_lock is held */
		spin_unlock_irq(&sched_lock, f);
		if (victim) {
			shown++;
			hung_report(victim, stalled);
			proc_put(victim);
		}
	}
	if (more) kprintf(KERN_WARN "hung: %d more tasks stalled, not shown\n", more);
	kwatch_kill_sweep(now);
}

/* ── boot self-test ───────────────────────────────────────────────────────────
 * Drives the exact decision path (hung_eval) and report path against a
 * synthetic, off-runqueue task so detection is proven deterministically — no 20s
 * wait, no real task wedged. Asserts the detector fires on a stale uninterruptible
 * block, then that it leaves a progressing task and a healthy interruptible
 * sleeper alone. Panics on a logic error; emits a greppable marker on success.
 * ─────────────────────────────────────────────────────────────────────────── */
void hung_task_selftest(void)
{
	/* A fixed local timeout, independent of the cmdline-derived production value
	 * (which is read lazily once IRQs/cmdline are up) — so the self-test can run
	 * this early and never consume/cache the configured value. */
	const uint64_t to = 20 * TIMER_HZ;

	process_t t;
	memset(&t, 0, sizeof(t));
	t.magic = PROC_MAGIC;
	t.pid   = 0x7e57;
	memcpy(t.name, "hungtest", 9);
	t.last_syscall = -1;

	/* (1) uninterruptible block, ticks frozen, window opened a full timeout ago. */
	uint64_t now   = kernel_ticks;
	t.hung_watch       = 1;
	t.state            = PROC_SLEEPING;
	t.ticks            = 1;
	t.hung_run_ticks   = 1;
	t.hung_stall_start = now - to;
	uint64_t before = hung_task_count;
	uint64_t stalled = hung_eval(&t, now, to);
	if (!stalled) kpanic("hungtask: stale block not flagged");
	hung_report(&t, stalled);
	if (hung_task_count != before + 1) kpanic("hungtask: detector did not fire");

	/* (2) the same task, but it ran since the baseline → no report, window clears. */
	t.ticks = 2;
	if (hung_eval(&t, now, to) != 0) kpanic("hungtask: progress not recognized");
	if (t.hung_stall_start != 0)     kpanic("hungtask: window not cleared on progress");

	/* (3) healthy interruptible sleeper (pause/recv/waitpid shape): never watched,
	 *     so even a stale window must not report. */
	t.hung_watch       = 0;
	t.state            = PROC_SLEEPING;
	t.ticks            = 5;
	t.hung_run_ticks   = 5;
	t.hung_stall_start = now - to;
	if (hung_eval(&t, now, to) != 0) kpanic("hungtask: interruptible sleeper flagged");

	kprintf(KERN_INFO "hung: selftest ok, detector fired %lu times\n", hung_task_count);
}
