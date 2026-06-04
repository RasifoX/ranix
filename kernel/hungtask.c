/* ── hung-task detector (khungtaskd parity) ──────────────────────────────────
 * A always-on, cheap net for the failure that today just hangs in silence: a
 * task the scheduler believes will make progress that never does — a lost
 * wakeup, a missed reschedule IPI, an orphaned runqueue entry, a deadlock on an
 * uninterruptible kernel block. The existing sched_watchdog catches a *CPU*
 * wedged with IRQs off (wd_ticks frozen); this catches a *task* that is stuck
 * while its CPU keeps taking ticks.
 *
 * Progress is read straight off p->ticks, which the tick bumps every time a task
 * is the current one (Linux times this with last_switch_count). A watched task
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
#include <kernel/bug.h>
#include <kernel/lib/string.h>

extern volatile uint64_t kernel_ticks;
extern spinlock_t        sched_lock;
extern int               task_count(void);
/* proctab[i] with no internal lock — the caller must already hold sched_lock
 * (task_get() locks internally and would self-deadlock under our sweep). */
extern process_t        *task_peek_locked(int index);

#ifndef TIMER_HZ
#define TIMER_HZ 100
#endif

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
	if (!hung_watched(p) || p->ticks != p->hung_run_ticks) {
		p->hung_run_ticks   = p->ticks;   /* progressed, or not watched */
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
	if (s) kprintf("    %s+0x%lx  (0x%016lx)\n", s, off, pc);
	else   kprintf("    0x%016lx\n", pc);
	for (int i = 0; i < 16 && fp >= 0xFFFF000000000000UL && (fp & 7) == 0; i++) {
		uint64_t next = ((uint64_t *)fp)[0];
		uint64_t ret  = ((uint64_t *)fp)[1];
		if (!ret) break;
		off = 0;
		s = ksym_lookup(ret, &off);
		if (s) kprintf("    %s+0x%lx  (0x%016lx)\n", s, off, ret);
		else   kprintf("    0x%016lx\n", ret);
		if (next <= fp) break;
		fp = next;
	}
}

/* Loud, marked, greppable. Distinct from a panic: a hung task is a diagnosis, not
 * a reason to take the machine down — the rest of the system is still running. */
static void hung_report(process_t *p, uint64_t stalled_ticks)
{
	hung_task_count++;
	kprintf("\nHUNG TASK: pid=%d (%s) %s for %lus  last_syscall=%d\n",
	        (int)p->pid, p->name, state_name(p->state),
	        stalled_ticks / TIMER_HZ, p->last_syscall);
	kprintf("  call trace\n");
	hung_backtrace(p);
	kprintf("HUNG TASK: end pid=%d\n\n", (int)p->pid);
}

/* One sweep. Gated by the caller to run a few times a minute, so the O(nproc)
 * walk is negligible. Snapshots and advances each task's window under sched_lock
 * (state/ticks change concurrently); prints outside the lock. */
void hung_task_check(void)
{
	uint64_t to = hung_timeout_ticks();
	if (!to) return;   /* disabled */

	uint64_t now = kernel_ticks;
	int n = task_count();
	for (int i = 0; i < n; i++) {
		uint64_t f = spin_lock_irq(&sched_lock);
		process_t *p = task_peek_locked(i);
		uint64_t stalled = (p && p->magic == PROC_MAGIC) ? hung_eval(p, now, to) : 0;
		process_t *victim = stalled ? p : NULL;
		spin_unlock_irq(&sched_lock, f);
		if (victim) hung_report(victim, stalled);
	}
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

	kprintf("hungtask: selftest ok (detector fired, count=%lu)\n", hung_task_count);
}
