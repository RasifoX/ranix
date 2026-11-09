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
static void kwatch_kill_sweep(uint64_t now);

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

static const char *state_name(uint64_t st)
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

static void hung_backtrace(process_t *p)
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

/* Loud, marked, greppable. Distinct from a panic: a hung task is a diagnosis, not
 * a reason to take the machine down — the rest of the system is still running. */
static void hung_report(process_t *p, uint64_t stalled_ticks)
{
	hung_task_count++;
	kprintf(KERN_WARN "hung: task pid=%d (%s) %s for %lu s, last_syscall=%d\n",
	        (int)p->pid, p->name, state_name(p->state),
	        stalled_ticks / TIMER_HZ, p->last_syscall);
	if (p->state == PROC_READY) {     /* runnable and not run: is it on a queue at all? */
		int where = -1;
		for (int i = 0; i < smp_ncpus && where < 0; i++) {
			int guard = 0;
			for (process_t *q = all_cpus[i] ? all_cpus[i]->rq_head : NULL; q && guard < 4096; q = q->sched_next, guard++)
				if (q == p) { where = i; break; }
		}
		int cur = -1;
		for (int i = 0; i < smp_ncpus; i++) if (all_cpus[i] && all_cpus[i]->current == p) cur = i;
		kprintf(KERN_WARN "hung: in_rq=%d on_cpu=%d queued_on=%d current_on=%d\n",
		        (int)p->in_rq, (int)p->on_cpu, where, cur);
		static uint64_t last_dump;
		if (kernel_ticks - last_dump > 5 * TIMER_HZ) {
			last_dump = kernel_ticks;
			for (int i = 0; i < smp_ncpus; i++) {
				cpu_context_t *c = all_cpus[i];
				if (!c) continue;
				process_t *q = c->current;
				int n = 0;
				for (process_t *r = c->rq_head; r && n < 4096; r = r->sched_next) n++;
				kprintf(KERN_WARN "hung: cpu%d current pid=%d (%s) state=%d syscall=%d rq=%d slow=%d resched=%d wd=%lu\n", i,
				        q ? (int)q->pid : -1, q ? q->name : "-", q ? (int)q->state : -1,
				        q ? q->last_syscall : -1, n, c->tick_slow, c->need_resched, (unsigned long)c->wd_ticks);
				if (q && q->pid) hung_backtrace(q);
			}
		}
	}
	kprintf(KERN_WARN "hung: call trace\n");
	hung_backtrace(p);
	kprintf(KERN_WARN "hung: end pid=%d\n", (int)p->pid);
}

/* One sweep. Gated by the caller to run a few times a minute, so the O(nproc)
 * walk is negligible. Snapshots and advances each task's window under sched_lock
 * (state/ticks change concurrently); prints outside the lock. */
void hung_task_check(void)
{
	uint64_t to = hung_timeout_ticks();
	if (!to) return;   /* disabled */

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

/* ── kill-always-wins watch ───────────────────────────────────────────────────
 * Two invariants the 2026-11-08 Casper wedge broke (an ioctl looping forever
 * while holding drm_lock; SIGKILL lost; shutdown waited on it for good):
 *   kill:   a task with SIGKILL pending must be gone within KW_KILL_SECS.
 *   kstall: a task must not run inside one syscall for KW_STALL_SECS without
 *           giving the CPU up while someone waits for it (or while it is killed).
 * Ranix cannot unwind a kernel loop that never checks, so the watch cannot fix a
 * breach; it names it (pid, syscall, backtrace) in the log, in a ring the rescue
 * channel serves (kernel/rescue.c), and in counters a test asserts stay at 0.
 * ─────────────────────────────────────────────────────────────────────────── */
#include <kernel/kout.h>
#include <kernel/smp.h>

#define KW_KILL_SECS   10
#define KW_STALL_SECS  10
#define KW_RING        8

volatile uint64_t kwatch_unkillable_count;
volatile uint64_t kwatch_kstall_count;

struct kw_event { uint64_t at; int pid; int sys; uint32_t secs; char kind; char name[16]; };
static struct kw_event kw_ring[KW_RING];
static volatile uint32_t kw_ring_n;
static spinlock_t kw_lock = SPINLOCK_INIT;

static void kw_note(char kind, process_t *p, uint32_t secs)
{
	uint64_t f = spin_lock_irq(&kw_lock);
	struct kw_event *e = &kw_ring[kw_ring_n++ % KW_RING];
	e->at = kernel_ticks; e->pid = (int)p->pid; e->sys = p->last_syscall; e->secs = secs; e->kind = kind;
	int i = 0;
	for (; i < 15 && p->name[i]; i++) e->name[i] = p->name[i];
	e->name[i] = 0;
	spin_unlock_irq(&kw_lock, f);
}

static struct { process_t *task; uint64_t sw, since, waited; uint8_t told; } kw_cpu[MAX_CPUS];

/* From sched_tick_local on every CPU, for the task the tick interrupted. */
void kwatch_tick_local(process_t *cur, cpu_context_t *cc)
{
	int c = (int)cc->cpu_id;
	if (c < 0 || c >= MAX_CPUS) return;
	if (!cur || !cur->pid || !cur->in_syscall) { kw_cpu[c].task = NULL; return; }
	if (kw_cpu[c].task != cur || kw_cpu[c].sw != cur->hung_switches) {
		kw_cpu[c].task = cur; kw_cpu[c].sw = cur->hung_switches;
		kw_cpu[c].since = kernel_ticks; kw_cpu[c].waited = 0; kw_cpu[c].told = 0;
		return;
	}
	/* Only a stall someone pays for: another task queued here, or the task killed.
	 * Timed in kernel_ticks, not in this CPU's tick count: a guest or a slowed
	 * tick delivers fewer local ticks than wall time passes. */
	int dead = (cur->sig_pending & (1UL << (9 - 1))) != 0;
	if (!(cc->rq_head || dead)) { kw_cpu[c].waited = 0; return; }
	if (!kw_cpu[c].waited) kw_cpu[c].waited = kernel_ticks ? kernel_ticks : 1;
	if (!kw_cpu[c].told && kernel_ticks - kw_cpu[c].waited >= (uint64_t)KW_STALL_SECS * TIMER_HZ) {
		kw_cpu[c].told = 1;
		kwatch_kstall_count++;
		uint32_t secs = (uint32_t)((kernel_ticks - kw_cpu[c].since) / TIMER_HZ);
		kw_note('S', cur, secs);
		kprintf(KERN_ERR "kstall: cpu%d pid=%d (%s) in syscall %d for %u s without yielding%s\n",
		        c, (int)cur->pid, cur->name, cur->last_syscall, secs,
		        dead ? ", SIGKILL pending: UNKILLABLE" : "");
	}
}

/* Backtrace of a task that is not on a CPU, into o (one frame per line). */
void kwatch_backtrace(process_t *p, kout_t *o)
{
	if (p->on_cpu) { kout_s(o, "    (running on a cpu: no saved frame)\n"); return; }
	arch_context_t *ctx = p->ctx;
	if (!ctx) return;
#if defined(__x86_64__)
	uint64_t pc = ctx->rip, fp = ctx->rbp;
#else
	uint64_t pc = ctx->elr, fp = ctx->regs[29];
#endif
	for (int i = 0; i < 12; i++) {
		uint64_t off = 0;
		const char *s = ksym_lookup(pc, &off);
		kout_s(o, "    ");
		if (s) { kout_s(o, s); kout_s(o, "+"); kout_x(o, off); } else kout_x(o, pc);
		kout_c(o, '\n');
		if (fp < 0xFFFF000000000000UL || (fp & 7)) break;
		uint64_t next = ((uint64_t *)fp)[0];
		pc = ((uint64_t *)fp)[1];
		if (!pc || next <= fp) break;
		fp = next;
	}
}

/* From hung_task_check (~every 5 s): SIGKILL posted and the task still here. */
static void kwatch_kill_sweep(uint64_t now)
{
	int n = task_count();
	for (int i = 0; i < n; i++) {
		uint64_t f = spin_lock_irq(&sched_lock);
		process_t *p = task_peek_locked(i), *victim = NULL;
		uint32_t secs = 0;
		if (p && p->magic == PROC_MAGIC && p->pid &&
		    p->state != PROC_ZOMBIE && p->state != PROC_DEAD &&
		    (p->sig_pending & (1UL << (9 - 1)))) {
			if (!p->kill_posted_at) p->kill_posted_at = now;
			if (now - p->kill_posted_at >= (uint64_t)KW_KILL_SECS * TIMER_HZ &&
			    (!p->kill_reported_at || now - p->kill_reported_at >= 60 * TIMER_HZ)) {
				p->kill_reported_at = now;
				secs = (uint32_t)((now - p->kill_posted_at) / TIMER_HZ);
				victim = p;
				victim->refcount++;
			}
		}
		spin_unlock_irq(&sched_lock, f);
		if (!victim) continue;
		kwatch_unkillable_count++;
		kw_note('K', victim, secs);
		kprintf(KERN_ERR "unkillable: pid=%d (%s) ignored SIGKILL for %u s, state=%s in_syscall=%d syscall=%d\n",
		        (int)victim->pid, victim->name, secs, state_name(victim->state),
		        victim->in_syscall, victim->last_syscall);
		hung_backtrace(victim);
		proc_put(victim);
	}
}

/* Rescue/status text: counters, the event ring, and every task now breaking an
 * invariant (killed but alive, or in a long syscall), with backtraces. */
void kwatch_report(kout_t *o, int with_bt)
{
	kout_s(o, "kwatch unkillable="); kout_u(o, kwatch_unkillable_count);
	kout_s(o, " kstall="); kout_u(o, kwatch_kstall_count);
	kout_s(o, " hung="); kout_u(o, hung_task_count); kout_c(o, '\n');
	uint64_t f = spin_lock_irq(&kw_lock);
	uint32_t n = kw_ring_n, first = n > KW_RING ? n - KW_RING : 0;
	struct kw_event ev[KW_RING];
	for (uint32_t i = first; i < n; i++) ev[i - first] = kw_ring[i % KW_RING];
	spin_unlock_irq(&kw_lock, f);
	for (uint32_t i = 0; i < n - first; i++) {
		kout_s(o, ev[i].kind == 'K' ? "event unkillable" : "event kstall");
		kout_s(o, " ago_s="); kout_u(o, (kernel_ticks - ev[i].at) / TIMER_HZ);
		kout_s(o, " pid="); kout_u(o, (uint64_t)ev[i].pid);
		kout_s(o, " comm="); kout_s(o, ev[i].name);
		kout_s(o, " syscall="); kout_i(o, ev[i].sys);
		kout_s(o, " secs="); kout_u(o, ev[i].secs); kout_c(o, '\n');
	}
	uint64_t now = kernel_ticks;
	int nt = task_count(), shown = 0;
	for (int i = 0; i < nt && shown < 16; i++) {
		f = spin_lock_irq(&sched_lock);
		process_t *p = task_peek_locked(i), *t = NULL;
		if (p && p->magic == PROC_MAGIC && p->pid && p->state != PROC_ZOMBIE && p->state != PROC_DEAD &&
		    ((p->sig_pending & (1UL << (9 - 1))) || p->hung_stall_start)) { t = p; t->refcount++; }
		spin_unlock_irq(&sched_lock, f);
		if (!t) continue;
		shown++;
		kout_s(o, "stuck pid="); kout_u(o, t->pid); kout_s(o, " comm="); kout_s(o, t->name);
		kout_s(o, " state="); kout_s(o, state_name(t->state));
		kout_s(o, " in_syscall="); kout_i(o, t->in_syscall);
		kout_s(o, " syscall="); kout_i(o, t->last_syscall);
		if (t->kill_posted_at) { kout_s(o, " killed_s="); kout_u(o, (now - t->kill_posted_at) / TIMER_HZ); }
		kout_c(o, '\n');
		if (with_bt) kwatch_backtrace(t, o);
		proc_put(t);
	}
	for (int c = 0; c < smp_ncpus && c < MAX_CPUS; c++) {
		process_t *t = kw_cpu[c].task;
		if (!t || now - kw_cpu[c].since < 2 * TIMER_HZ) continue;
		kout_s(o, "cpu"); kout_u(o, (uint64_t)c); kout_s(o, " in-syscall-without-yield s=");
		kout_u(o, (now - kw_cpu[c].since) / TIMER_HZ); kout_c(o, '\n');
	}
}
