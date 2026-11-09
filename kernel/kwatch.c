/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
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

#include <kernel/proc.h>
#include <kernel/hungtask.h>
#include <kernel/kwatch.h>
#include <kernel/hal.h>
#include <kernel/sync.h>
#include <kernel/ksyms.h>
#include <kernel/kprintf.h>
#include <kernel/kout.h>
#include <kernel/board.h>   /* TIMER_HZ */
#include <kernel/time.h>
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
	int dead = (cur->sig_pending & (1UL << (SIGKILL - 1))) != 0;
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
static void kwatch_backtrace(process_t *p, kout_t *o)
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
void kwatch_kill_sweep(uint64_t now)
{
	int n = task_count();
	for (int i = 0; i < n; i++) {
		uint64_t f = spin_lock_irq(&sched_lock);
		process_t *p = task_peek_locked(i), *victim = NULL;
		uint32_t secs = 0;
		if (p && p->magic == PROC_MAGIC && p->pid &&
		    p->state != PROC_ZOMBIE && p->state != PROC_DEAD &&
		    (p->sig_pending & (1UL << (SIGKILL - 1)))) {
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
		        (int)victim->pid, victim->name, secs, hung_state_name(victim->state),
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
		    ((p->sig_pending & (1UL << (SIGKILL - 1))) || p->hung_stall_start)) { t = p; t->refcount++; }
		spin_unlock_irq(&sched_lock, f);
		if (!t) continue;
		shown++;
		kout_s(o, "stuck pid="); kout_u(o, t->pid); kout_s(o, " comm="); kout_s(o, t->name);
		kout_s(o, " state="); kout_s(o, hung_state_name(t->state));
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
