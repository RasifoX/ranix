/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* ── Pressure stall information: /proc/pressure/{cpu,memory,io} ──────────────
 * The share of time work could not run because it waited for a resource, in
 * Linux's format and meaning, so tools that read it (systemd-oomd, earlyoom,
 * the slot command, a desktop's "the machine is struggling") work unchanged:
 *   some  at least one task was stalled on the resource,
 *   full  stalled tasks existed and no CPU was running anything productive
 *         (for cpu, "full" is not defined system-wide; it reads 0 as on Linux).
 * Stalls: memory = direct reclaim on an allocation path and swap-ins (kswapd's
 * own background work is not a stall, as on Linux); io = time in a block
 * device read or write (the drivers complete synchronously, so the task is
 * waiting on the device the whole time); cpu = a CPU has runnable tasks
 * queued behind the one it runs.
 * How: tasks mark themselves (psi_enter/psi_leave, a count per resource); the
 * BSP's tick samples the state, weighted by the ticks it covers (a slowed idle
 * tick covers several, and there is no stall then). Every 2 s the averages
 * over 10, 60 and 300 s move by Linux's exponential decay. Totals are in µs.
 * Not done: Linux's per-cgroup pressure files and poll() triggers. */

#include <kernel/psi.h>
#include <kernel/proc.h>
#include <kernel/sched.h>
#include <kernel/hal.h>
#include <kernel/board.h>   /* TIMER_HZ */
#include <kernel/sync.h>
#include <kernel/smp.h>
#include <kernel/lib/string.h>

static volatile int n_stalled[PSI_RES_N];              /* tasks marked, per resource */

/* per resource: some and full; totals in ticks, averages in percent << 16 */
struct psi_series { uint64_t total, last; uint64_t avg[3]; };
static struct psi_series series[PSI_RES_N][2];
static uint64_t          psi_ticks, psi_window_start;
static spinlock_t        psi_lock = SPINLOCK_INIT;

#define PSI_PERIOD (2 * TIMER_HZ)
/* e^(-2/10), e^(-2/60), e^(-2/300) in 1/2048, as Linux's EXP_10s/60s/300s */
static const uint64_t PSI_EXP[3] = { 1677, 1981, 2034 };

unsigned psi_enter(unsigned what)
{
	process_t *p = task_current();
	if (!p) return 0;
	unsigned prev = p->psi;
	unsigned add = what & ~prev;
	if (!add) return prev;
	p->psi = prev | add;
	if (add & PSI_MEM) __atomic_add_fetch(&n_stalled[PSI_RES_MEM], 1, __ATOMIC_RELAXED);
	if (add & PSI_IO)  __atomic_add_fetch(&n_stalled[PSI_RES_IO], 1, __ATOMIC_RELAXED);
	return prev;
}

void psi_leave(unsigned prev)
{
	process_t *p = task_current();
	if (!p) return;
	unsigned drop = p->psi & ~prev;
	if (!drop) return;
	p->psi = prev;
	if (drop & PSI_MEM) __atomic_sub_fetch(&n_stalled[PSI_RES_MEM], 1, __ATOMIC_RELAXED);
	if (drop & PSI_IO)  __atomic_sub_fetch(&n_stalled[PSI_RES_IO], 1, __ATOMIC_RELAXED);
}

static void psi_average(uint64_t window)
{
	for (int r = 0; r < PSI_RES_N; r++)
		for (int k = 0; k < 2; k++) {
			struct psi_series *s = &series[r][k];
			uint64_t d = s->total - s->last;
			s->last = s->total;
			uint64_t pct = window ? (d * (100ULL << 16)) / window : 0;   /* percent << 16 */
			for (int i = 0; i < 3; i++)
				s->avg[i] = (s->avg[i] * PSI_EXP[i] + pct * (2048 - PSI_EXP[i])) / 2048;
		}
}

/* A sample: reads other CPUs' current task and queue without their locks.
 * A stale read moves one tick of accounting, never memory: process structs
 * are heap objects that stay mapped. */
void psi_tick(uint64_t ticks)
{
	int some_cpu = 0, busy_mem = 0, busy_io = 0;
	for (int i = 0; i < smp_ncpus; i++) {
		cpu_context_t *c = all_cpus[i];
		if (!c) continue;
		if (c->rq_head) { some_cpu = 1; busy_mem = busy_io = 1; }   /* runnable: not stalled */
		process_t *cur = c->current;
		if (cur && cur->pid) {
			if (!(cur->psi & PSI_MEM)) busy_mem = 1;
			if (!(cur->psi & PSI_IO))  busy_io = 1;
		}
	}
	int mem = __atomic_load_n(&n_stalled[PSI_RES_MEM], __ATOMIC_RELAXED) > 0;
	int io  = __atomic_load_n(&n_stalled[PSI_RES_IO], __ATOMIC_RELAXED) > 0;

	uint64_t f = spin_lock_irq(&psi_lock);
	if (some_cpu) series[PSI_RES_CPU][0].total += ticks;
	if (mem) { series[PSI_RES_MEM][0].total += ticks; if (!busy_mem) series[PSI_RES_MEM][1].total += ticks; }
	if (io)  { series[PSI_RES_IO][0].total += ticks;  if (!busy_io)  series[PSI_RES_IO][1].total += ticks; }
	psi_ticks += ticks;
	if (psi_ticks - psi_window_start >= PSI_PERIOD) {
		psi_average(psi_ticks - psi_window_start);
		psi_window_start = psi_ticks;
	}
	spin_unlock_irq(&psi_lock, f);
}

static size_t put_s(char *b, size_t n, size_t sz, const char *s)
{
	while (*s && n + 1 < sz) b[n++] = *s++;
	return n;
}

static size_t put_u(char *b, size_t n, size_t sz, uint64_t v)
{
	char t[24]; int k = 0;
	do { t[k++] = (char)('0' + v % 10); v /= 10; } while (v);
	while (k && n + 1 < sz) b[n++] = t[--k];
	return n;
}

/* percent << 16 as "12.34" */
static size_t put_pct(char *b, size_t n, size_t sz, uint64_t v)
{
	uint64_t hund = (v * 100 + (1 << 15)) >> 16;
	n = put_u(b, n, sz, hund / 100);
	n = put_s(b, n, sz, ".");
	if (hund % 100 < 10) n = put_s(b, n, sz, "0");
	return put_u(b, n, sz, hund % 100);
}

int psi_show(int res, char *buf, size_t sz)
{
	if (res < 0 || res >= PSI_RES_N || !sz) return 0;
	struct psi_series s[2];
	uint64_t f = spin_lock_irq(&psi_lock);
	s[0] = series[res][0]; s[1] = series[res][1];
	spin_unlock_irq(&psi_lock, f);
	size_t n = 0;
	for (int k = 0; k < 2; k++) {
		n = put_s(buf, n, sz, k ? "full" : "some");
		static const char *const name[3] = { " avg10=", " avg60=", " avg300=" };
		for (int i = 0; i < 3; i++) { n = put_s(buf, n, sz, name[i]); n = put_pct(buf, n, sz, s[k].avg[i]); }
		n = put_s(buf, n, sz, " total=");
		n = put_u(buf, n, sz, s[k].total * (1000000 / TIMER_HZ));
		n = put_s(buf, n, sz, "\n");
	}
	buf[n] = 0;
	return (int)n;
}
