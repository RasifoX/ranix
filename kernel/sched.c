#include <kernel/proc.h>
#include <kernel/nsproxy.h>
#include <kernel/pid_namespace.h>
#include <kernel/user_namespace.h>
#include <kernel/capabilities.h>
#include <kernel/net/net.h>
#include <kernel/cgroup.h>
#include <kernel/config.h>
#include <kernel/hal.h>
#include <kernel/bug.h>
#include <kernel/cmdline.h>
#include <kernel/tick.h>
#include <kernel/elf.h>
#include <kernel/kprintf.h>
#include <kernel/mm/heap.h>
#include <kernel/irq.h>
#include <kernel/mm/buddy.h>
#include <kernel/mm/mmu.h>
#include <kernel/mm/vm.h>
#include <kernel/vfs.h>
#include <kernel/sync.h>
#include <kernel/lib/string.h>
#include <kernel/smp.h>
#include <asm/mmu.h>

extern void futex_wake_addr(uint32_t *uaddr);

static process_t **proctab;
static int         proc_cap = 0;
static process_t   idle_proc;
static uint8_t     idle_kstack[KSTACK_SIZE] __attribute__((aligned(16)));
static uint8_t     irq_stack[KSTACK_SIZE] __attribute__((aligned(16)));
static int         proc_cnt  = 0;

#define PROCTAB_INIT_CAP 64

static int proctab_grow(void)
{
	int new_cap = proc_cap ? proc_cap * 2 : PROCTAB_INIT_CAP;
	process_t **nb = kmalloc((size_t)new_cap * sizeof(process_t *));
	if (!nb) return -1;
	if (proctab) {
		memcpy(nb, proctab, (size_t)proc_cap * sizeof(process_t *));
		kfree(proctab);
	}
	memset(nb + proc_cap, 0, (size_t)(new_cap - proc_cap) * sizeof(process_t *));
	proctab = nb;
	proc_cap = new_cap;
	return 0;
}

static int proctab_ensure(void)
{
	if (proc_cnt < proc_cap) return 0;
	return proctab_grow();
}

/* Monotonic climbing pid allocation: hand out fresh pids up
 * to PID_MAX before ever reusing a freed one. Quick reuse is unsafe — a stale
 * reference to a dead pid (a `timeout` parent's liveness probe / kill, musl's
 * __thread_list_lock keyed on a tid) would then land on an unrelated process
 * that reused the number. Only after the space fills do we wrap and reuse. */
#define PID_MAX 32768
static int last_pid = 0;
static int alloc_pid(void)
{
	if (proc_cnt < PID_MAX && proctab_ensure() == 0) {
		last_pid = proc_cnt;
		return proc_cnt++;
	}
	for (int n = 1; n < proc_cnt; n++) {
		int i = last_pid + n;
		if (i >= proc_cnt) i -= (proc_cnt - 1);   /* wrap, skipping slot 0 */
		if (i == 0) i = 1;
		if (!proctab[i]) { last_pid = i; return i; }
	}
	return -1;
}

#define KSTACK_CANARY 0xDEADBEEFCAFEBABEULL

static int kstack_size_val  = KSTACK_SIZE;
static int kstack_order_val = 2; /* buddy order; 2 = 4 pages = 16384 bytes */

static uint8_t *kstack_alloc(void)
{
	void *p = buddy_alloc(kstack_order_val);
	if (!p) return NULL;
	uint8_t *ks = (uint8_t *)PHYS_TO_VIRT(p);
	*(uint64_t *)ks = KSTACK_CANARY;
	return ks;
}

/* Canary at the base (lowest address) of the kernel stack — the last word before
 * an overflow runs off the bottom. schedule() checks it per context switch; this
 * lets the common user-return path catch a breach in the same trap that caused
 * it. (idle/pid 0 uses a static stack without the canary — skip it.) */
void kstack_check(void)
{
	process_t *p = task_current();
	if (p && p->pid != 0 && p->kstack && *(uint64_t *)p->kstack != KSTACK_CANARY)
		kpanic("kstack overflow detected");
}



/*
 * sched_lock is the single lock for everything scheduler-related: the per-CPU
 * runqueues, process state machine, the global process list, proctab/pid space,
 * pending-signal bits and the context switch. It is the scalability ceiling —
 * the runqueues are already per-CPU (cpu->rq_head/tail) but every queue op,
 * wakeup, signal post and switch contends this one lock.
 *
 * SPLITTING IT (per-rq locks) — design, deferred as too invasive to land safely:
 *
 *   Locks, in acquisition order (always low → high to avoid ABBA):
 *     L1  proc_lock      — global process list (->next), proctab[], pid alloc.
 *     L2  rq->lock       — one per CPU (cpu_context_t already reserves rq_lock):
 *                          rq_head/tail, in_rq, sched_next. For two rqs, lock the
 *                          lower cpu_id first.
 *     L3  p->pi_lock     — per-task: state, on_cpu, sleeping_on_queue,
 *                          wake_deadline, sig_pending/sig_mask, itimer fields.
 *   A task's pi_lock nests inside its target rq->lock; rq->lock nests inside
 *   proc_lock. Never take proc_lock while holding an rq->lock.
 *
 *   Field ownership:
 *     - rq_head/tail/in_rq/sched_next  → that CPU's rq->lock.
 *     - state/on_cpu/sig_pending/...   → p->pi_lock (today these ride sched_lock).
 *     - ->next list + proctab + pids   → proc_lock.
 *
 *   wake/enqueue (task_wake, task_signal*, tick deadline): take p->pi_lock, set
 *   state READY; if !on_cpu, pick a target CPU and enqueue. pick_cpu() today
 *   reads every CPU's rq_len — under per-rq locks that must become a lockless
 *   snapshot (cache an atomic rq->nr_running per CPU and read it unlocked; exact
 *   length is only a hint) so enqueue locks just the chosen rq->lock, not all of
 *   them. Order: pi_lock → target rq->lock.
 *
 *   schedule(): lock this CPU's rq->lock, pop. On empty, work-steal: snapshot
 *   nr_running unlocked to choose a victim, then lock victim rq->lock (lower id
 *   first vs self) and re-check before popping (the task may have left). The
 *   prev requeue (sched_requeue_prev) takes prev->pi_lock then this rq->lock.
 *
 *   Cross-CPU signal/group/pgrp walks take proc_lock to traverse ->next, then
 *   per-target pi_lock (+ that target's rq->lock only if it must be enqueued) —
 *   so a long walk no longer blocks every other CPU's schedule().
 *
 *   The hard parts that make this unsafe to ship without a soak: pick_cpu/steal
 *   reading all runqueues, the three-lock enqueue-from-walk path, and keeping
 *   the prev-requeue / dying-kstack handoff in schedule() atomic across the new
 *   lock boundaries. A single missed invariant is a rare runqueue corruption or
 *   lost wakeup the suite won't deterministically catch — so until that soak
 *   exists, the one lock stays.
 */
spinlock_t sched_lock = SPINLOCK_INIT;
static int sched_quantum_val = SCHED_QUANTUM;


static tick_fn_t *tick_cbs;
static int        tick_cb_cnt = 0;
static int        tick_cb_cap = 0;

static void itimer_charge_cpu(process_t *p);

void tick_register(tick_fn_t fn)
{
	if (tick_cb_cnt >= tick_cb_cap) {
		int nc = tick_cb_cap ? tick_cb_cap * 2 : 8;
		tick_fn_t *nb = kmalloc((size_t)nc * sizeof(tick_fn_t));
		if (!nb) return;
		if (tick_cbs) { memcpy(nb, tick_cbs, (size_t)tick_cb_cnt * sizeof(tick_fn_t)); kfree(tick_cbs); }
		tick_cbs = nb; tick_cb_cap = nc;
	}
	tick_cbs[tick_cb_cnt++] = fn;
}

/* Runs on CPU0 only. A CPU whose wd_ticks froze since the last sweep took no
 * timer interrupt for ~2.5s — wedged with IRQs masked (spinlock deadlock or a
 * stuck IRQ-off section). Report which CPU, where, and on what. */
static void sched_watchdog(void)
{
	extern int smp_ncpus;
	extern cpu_context_t *all_cpus[];
	static uint64_t last_seen[MAX_CPUS];
	static unsigned frozen[MAX_CPUS];
	static unsigned n;
	static int warm;

	if ((++n & 0xFF) != 0) return;   /* ~every 256 ticks (2.56s @100Hz) */

	if (warm < 2) {
		warm++;
		for (int i = 0; i < smp_ncpus; i++)
			if (all_cpus[i]) last_seen[i] = all_cpus[i]->wd_ticks;
		return;
	}

	for (int i = 0; i < smp_ncpus; i++) {
		cpu_context_t *c = all_cpus[i];
		if (!c || !c->online) continue;
		process_t *cur = c->current;
		/* An idle CPU halted in WFI/HLT legitimately takes no tick until an IPI
		 * wakes it (x86 secondaries lack a per-CPU timer) — only a CPU running
		 * real work with frozen ticks is wedged. */
		if (cur && cur->pid != 0 && c->wd_ticks == last_seen[i])
			frozen[i]++;
		else
			frozen[i] = 0;
		/* require sustained freeze across sweeps: a transient TCG host-starvation
		 * stall resolves and resets; a real IRQ-off deadlock stays frozen. */
		if (frozen[i] >= 3) {
			kprintf("WATCHDOG: cpu%d wedged, where=%s pid=%d (%s)\n",
			        i, c->wd_where ? c->wd_where : "?",
			        (int)cur->pid, cur->name);
			kpanic("cpu wedged (watchdog)");
		}
		last_seen[i] = c->wd_ticks;
	}
}

void tick_notify(void)
{
	/* sched_tick always runs on every CPU */
	sched_tick();
	/* Global callbacks (tcp_tick, timerfd_tick) run on CPU0 only */
	if (arch_get_cpu_ctx()->cpu_id == 0) {
		for (int i = 1; i < tick_cb_cnt; i++)
			tick_cbs[i]();
		sched_watchdog();
	}
}

/* Per-CPU tick: scheduler accounting only, no global bookkeeping. Runs on
 * every CPU from its own local timer; the global walk (kernel_ticks, sleeper
 * wakeups, itimers, watchdog) stays on the BSP's tick_notify. */
void sched_tick_local(void)
{
	arch_get_cpu_ctx()->wd_ticks++;
	process_t *current = task_current();
	if (!current) return;
	current->ticks++;
	itimer_charge_cpu(current);
	if (current->pid != 0) {
		if (--current->ticks_remaining <= 0)
			arch_get_cpu_ctx()->need_resched = 1;
	} else if (arch_get_cpu_ctx()->rq_head) {
		arch_get_cpu_ctx()->need_resched = 1;
	}
}

/* The cpu0-only sched_watchdog cannot report a wedge on cpu0 itself — its own
 * tick is what runs the check, so a cpu0 lockup silently freezes the detector.
 * cpu1 runs this twin to close that blind spot. Own statics, single runner →
 * lock-free. */
static void sched_watchdog_remote(void)
{
	extern int smp_ncpus;
	extern cpu_context_t *all_cpus[];
	static uint64_t last_seen[MAX_CPUS];
	static unsigned frozen[MAX_CPUS];
	static unsigned n, warm;

	if ((++n & 0x7F) != 0) return;
	if (warm < 2) {
		warm++;
		for (int i = 0; i < smp_ncpus; i++)
			if (all_cpus[i]) last_seen[i] = all_cpus[i]->wd_ticks;
		return;
	}
	for (int i = 0; i < smp_ncpus; i++) {
		cpu_context_t *c = all_cpus[i];
		if (!c || !c->online) continue;
		process_t *cur = c->current;
		if (cur && cur->pid != 0 && c->wd_ticks == last_seen[i]) frozen[i]++;
		else frozen[i] = 0;
		if (frozen[i] >= 3) {
			kprintf("WATCHDOG: cpu%d wedged in %s, pid=%d (%s) syscall=%d\n",
			        i, c->wd_where ? c->wd_where : "?",
			        cur ? (int)cur->pid : -1, cur ? cur->name : "-",
			        cur ? cur->last_syscall : -1);
			kpanic("cpu wedged (cross-cpu watchdog)");
		}
		last_seen[i] = c->wd_ticks;
	}
}

void tick_notify_local(void)
{
	sched_tick_local();
	if (arch_get_cpu_ctx()->cpu_id == 1) sched_watchdog_remote();
}

volatile uint64_t kernel_ticks  = 0;

/* Bounded tick: instead of scanning every process each tick, two gates tell
 * sched_tick whether the O(n) walk can have any work.
 *
 *  sched_next_wake — earliest armed wake_deadline (0 = none). Arming only ever
 *  lowers it (sched_note_wake_deadline), so it is never later than the true
 *  earliest deadline: a deadline can't be missed. Stale-low at worst costs one
 *  walk that recomputes it exactly. Updated lock-free (CAS-min) so poll_block,
 *  which arms wake_deadline under poll_wq.lock rather than sched_lock, can use it.
 *
 *  sched_itimer_armed — count of processes with itimer_value != 0 (ITIMER_REAL).
 *  itimer_value is a per-tick countdown, so its decrement loop must run while any
 *  are armed; the count is maintained at every transition under sched_lock. */
static volatile uint64_t sched_next_wake;
static int               sched_itimer_armed;

void sched_note_wake_deadline(uint64_t deadline)
{
	if (!deadline) return;
	uint64_t cur;
	do {
		cur = sched_next_wake;
		if (cur && cur <= deadline) return;
	} while (atomic_cmpxchg64(&sched_next_wake, cur, deadline) != cur);
}

void sched_itimer_account(uint64_t old_val, uint64_t new_val)
{
	if (!old_val && new_val)      sched_itimer_armed++;
	else if (old_val && !new_val) sched_itimer_armed--;
}

/* Per-CPU runqueue helpers — caller must hold sched_lock */

static void cpu_rq_add(cpu_context_t *cpu, process_t *p)
{
	if ((uint64_t)p < 0xFFFF000000000000ULL) {
		kprintf("[DBG] cpu_rq_add: bad p=%p cpu%d\n", p, (int)cpu->cpu_id);
		kpanic("cpu_rq_add: invalid process ptr");
	}
	if (p->in_rq) return;   /* already queued — guard against double-enqueue corruption */
	p->in_rq = 1;
	p->sched_next = NULL;
	if (!cpu->rq_tail) {
		cpu->rq_head = cpu->rq_tail = p;
	} else if (p->sched_policy != SCHED_NORMAL) {
		/* real-time (SCHED_FIFO/RR): run ahead of normal tasks */
		p->sched_next = cpu->rq_head;
		cpu->rq_head = p;
	} else {
		cpu->rq_tail->sched_next = p;
		cpu->rq_tail = p;
	}
}

static process_t *cpu_rq_pop(cpu_context_t *cpu)
{
	if (!cpu->rq_head) return NULL;
	process_t *p = cpu->rq_head;
	if ((uint64_t)p < 0xFFFF000000000000ULL) {
		kprintf("[DBG] cpu_rq_pop: bad head %p on cpu%d rq_tail=%p\n",
		        p, (int)cpu->cpu_id, cpu->rq_tail);
		cpu->rq_head = cpu->rq_tail = NULL;
		kpanic("cpu_rq_pop: corrupt rq_head");
	}
	process_t *nxt = p->sched_next;
	if (nxt && (uint64_t)nxt < 0xFFFF000000000000ULL) {
		kprintf("[DBG] cpu_rq_pop: bad sched_next %p in proc pid=%lld cpu%d\n",
		        nxt, (long long)p->pid, (int)cpu->cpu_id);
		kpanic("cpu_rq_pop: corrupt sched_next");
	}
	cpu->rq_head = nxt;
	if (!cpu->rq_head) cpu->rq_tail = NULL;
	p->sched_next = NULL;
	p->in_rq = 0;
	return p;
}

/* Count processes in a CPU's runqueue */
static int cpu_rq_len(cpu_context_t *cpu)
{
	int n = 0;
	for (process_t *p = cpu->rq_head; p; p = p->sched_next) n++;
	return n;
}

/* Pick the least-loaded online CPU (both arches — without this, x86 piled
 * every task onto the CPU that forked it while the others sat idle). */
static cpu_context_t *pick_cpu(void)
{
	extern int smp_ncpus;
	extern cpu_context_t *all_cpus[];
	cpu_context_t *best = arch_get_cpu_ctx();
	int best_len = cpu_rq_len(best);
	for (int i = 0; i < smp_ncpus; i++) {
		cpu_context_t *c = all_cpus[i];
		if (!c || !c->online) continue;
		int l = cpu_rq_len(c);
		if (l < best_len) { best = c; best_len = l; }
	}
	return best;
}

/* Internal: must hold sched_lock */
static void runqueue_add(process_t *p)
{
	if (p->pid == 0) return;
	cpu_context_t *cpu = pick_cpu();
	cpu_rq_add(cpu, p);
	cpu->need_resched = 1;
	if (cpu != arch_get_cpu_ctx()) {
		extern void smp_ipi_resched(int);
		int cid = (int)cpu->cpu_id;
		smp_ipi_resched(cid);
		__asm__ volatile("" ::: "memory");
	}
}

/* Pop from current CPU; if empty, try work-stealing */
static process_t *runqueue_pop(void)
{
	cpu_context_t *cpu = arch_get_cpu_ctx();
	process_t *p = cpu_rq_pop(cpu);
	if (p) return p;

	/* work stealing: find busiest CPU */
	extern int smp_ncpus;
	extern cpu_context_t *all_cpus[];
	cpu_context_t *victim = NULL;
	int max_len = 0;  /* steal any queued task — a lone runnable task on a busy
	                     CPU must not starve while this CPU is idle */
	for (int i = 0; i < smp_ncpus; i++) {
		cpu_context_t *c = all_cpus[i];
		if (!c || !c->online || c == cpu) continue;
		int l = cpu_rq_len(c);
		if (l > max_len) { victim = c; max_len = l; }
	}
	if (!victim) return NULL;
	return cpu_rq_pop(victim);
}



/* ── scheduler ───────────────────────────────────────────── */

static void idle_loop(void)
{
	/* Reschedule the instant a task becomes runnable instead of sleeping in
	 * WFI/HLT until the next timer tick — yield/wake latency 10ms→µs. No lost
	 * wakeups: need_resched on an idle CPU is set by an interrupt (wakes WFI)
	 * or the requeue just before idling (caught on the first iteration). */
	while (1) {
		cpu_context_t *c = arch_get_cpu_ctx();
		if (c->need_resched) {
			c->need_resched = 0;   /* consume the hint; runqueue is the truth */
			arch_yield();
		} else {
			arch_halt();
		}
	}
}

void sched_init(void)
{
	process_t *p = &idle_proc;
	memset(p, 0, sizeof(*p));
	p->pid   = 0;
	p->state = PROC_RUNNING;
	p->on_cpu = 1;
	p->entry = idle_loop;
	p->next  = p;
	p->kstack = idle_kstack;
	p->ticks_remaining = sched_quantum_val;
	memcpy(p->name, "idle", 5);

	uint8_t *sp_top = idle_kstack + KSTACK_SIZE;
	arch_context_t *ctx = (arch_context_t *)(sp_top - sizeof(arch_context_t));
	arch_context_init(ctx, idle_loop, (uint64_t)sp_top);
	p->ctx = ctx;

	cpu_context_t *cpu = arch_get_cpu_ctx();
	cpu->self         = cpu;
	cpu->current      = p;
	cpu->kstack_top   = sp_top;
	cpu->irq_stack    = irq_stack + KSTACK_SIZE;
	cpu->cpu_id       = 0;
	cpu->magic        = 0xCAFEBABE;
	cpu->rq_head      = NULL;
	cpu->rq_tail      = NULL;
	cpu->online       = 1;
	cpu->need_resched = 0;
	cpu->idle         = p;
	all_cpus[0]       = cpu;

	proctab_grow();
	proctab[0] = p;
	proc_cnt   = 1;

	{
		char buf[16];
		if (cmdline_get("sched_quantum", buf, sizeof(buf))) {
			int v = 0;
			for (const char *s = buf; *s >= '0' && *s <= '9'; s++)
				v = v * 10 + (*s - '0');
			if (v > 0) sched_quantum_val = v;
		}
		if (cmdline_get("kstack_size", buf, sizeof(buf))) {
			int v = 0;
			for (const char *s = buf; *s >= '0' && *s <= '9'; s++)
				v = v * 10 + (*s - '0');
			/* must be a multiple of PAGE_SIZE and at least 4096 */
			if (v >= 4096 && (v & (v - 1)) == 0) {
				int pages = v / 4096;
				int order = 0;
				while ((1 << order) < pages) order++;
				kstack_size_val  = (1 << order) * 4096;
				kstack_order_val = order;
			}
		}
	}

	tick_register(sched_tick);

	extern void tcp_tick(void);
	tick_register(tcp_tick);

	extern void timerfd_tick(void);
	tick_register(timerfd_tick);
}

/* Charge the running process's CPU-time interval timers for this tick.
 * ITIMER_PROF counts every tick the process is current (user + kernel time);
 * ITIMER_VIRTUAL counts only ticks spent in user mode (not in a syscall).
 * Caller holds no lock; runs in timer-IRQ context with the process current. */
static void itimer_charge_cpu(process_t *p)
{
	if (!p || p->pid == 0) return;
	int in_user = !p->in_syscall;
	/* user/system CPU-time split for times(2) and cputime clocks. Lock-free:
	 * a process is current on at most one CPU, so only this CPU writes these. */
	if (in_user) p->utime_ticks++; else p->stime_ticks++;
	if (!p->vtimer_value && !p->ptimer_value) return;
	uint64_t f = spin_lock_irq(&sched_lock);   /* set under the lock task_signal uses */
	if (in_user && p->vtimer_value && --p->vtimer_value == 0) {
		p->vtimer_value = p->vtimer_interval;
		p->sig_pending |= (1ULL << (26 - 1));   /* SIGVTALRM */
	}
	if (p->ptimer_value && --p->ptimer_value == 0) {
		p->ptimer_value = p->ptimer_interval;
		p->sig_pending |= (1ULL << (27 - 1));   /* SIGPROF */
	}
	spin_unlock_irq(&sched_lock, f);
}

void sched_tick(void)
{
	kernel_ticks++;
	arch_get_cpu_ctx()->wd_ticks++;   /* watchdog heartbeat (timer-IRQ driven) */
	process_t *current = task_current();
	if (!current) return;

	current->ticks++;
	itimer_charge_cpu(current);

	uint64_t f = spin_lock_irq(&sched_lock);

	if (current->pid != 0) {
		if (--current->ticks_remaining <= 0)
			arch_get_cpu_ctx()->need_resched = 1;
	} else if (arch_get_cpu_ctx()->rq_head) {
		arch_get_cpu_ctx()->need_resched = 1;
	}

	/* Bounded: skip the O(n) walk unless an ITIMER_REAL is counting down or a
	 * sleeper's deadline has come due. The "wake every sleeper and re-check"
	 * blanket is gone — every blocking primitive is event-driven (woken by its
	 * producer) or deadline-armed. */
	int do_itimer   = sched_itimer_armed > 0;
	int do_deadline = sched_next_wake && kernel_ticks >= sched_next_wake;
	if (do_itimer || do_deadline) {
		uint64_t next_wake = 0;   /* recompute the deadline gate while we walk */
		process_t *sentinel = proctab[0];
		for (process_t *t = sentinel->next; t && t != sentinel; t = t->next) {
			if (do_itimer && t->itimer_value > 0) {
				t->itimer_value--;
				if (t->itimer_value == 0) {
					t->itimer_value = t->itimer_interval;
					if (!t->itimer_value) sched_itimer_armed--;
					t->sig_pending |= (1ULL << (14 - 1));
					if (t->state == PROC_SLEEPING) {
						t->state = PROC_READY;
						if (!t->on_cpu) runqueue_add(t);
						arch_get_cpu_ctx()->need_resched = 1;
					}
				}
			}
			if (t->wake_deadline) {
				if (kernel_ticks >= t->wake_deadline) {
					t->wake_deadline = 0;
					if (t->state == PROC_SLEEPING) {
						t->state = PROC_READY;
						if (!t->on_cpu) runqueue_add(t);
						arch_get_cpu_ctx()->need_resched = 1;
					}
				} else if (!next_wake || t->wake_deadline < next_wake) {
					next_wake = t->wake_deadline;
				}
			}
		}
		/* Publish the recomputed gate, but never raise it above a deadline a
		 * concurrent poll_block (arms outside sched_lock) lowered it to mid-walk. */
		uint64_t cur;
		do {
			cur = sched_next_wake;
			if (cur && (!next_wake || cur < next_wake)) break;
		} while (atomic_cmpxchg64(&sched_next_wake, cur, next_wake) != cur);
	}

	spin_unlock_irq(&sched_lock, f);
}

/* process lifecycle (proc_create/exec/fork/vfork/clone/find/wait) -> kernel/proc.c */

void sched_free_dying_kstack(void)
{
	cpu_context_t *cpu = arch_get_cpu_ctx();

	if (cpu->dying_kstack) {
		buddy_free(VIRT_TO_PHYS(cpu->dying_kstack), cpu->dying_kstack_order);
		cpu->dying_kstack = NULL;
	}
}

/*
 * Called from assembly AFTER the stack switch (mov sp, x0).  At that point
 * the previous process's kstack is no longer in use by this CPU, so it is
 * safe to make prev visible to other CPUs via the runqueue.
 */
void sched_requeue_prev(void)
{
	cpu_context_t *cpu = arch_get_cpu_ctx();
	process_t *p = cpu->pending_requeue;
	if (!p) return;
	cpu->pending_requeue = NULL;

	uint64_t f = spin_lock_irq(&sched_lock);
	p->on_cpu = 0;
	/* Only requeue a genuinely runnable process. A process that exited
	 * after being marked for requeue (PROC_DEAD/ZOMBIE) must not go back
	 * into the runqueue, or schedule() could resume its task_exit. */
	if (p->state == PROC_READY)
		runqueue_add(p);
	spin_unlock_irq(&sched_lock, f);
}

void schedule(void)
{
	arch_get_cpu_ctx()->wd_where = "schedule";
	uint64_t f = spin_lock_irq(&sched_lock);
	process_t *prev = task_current();

	cpu_context_t *cpu = arch_get_cpu_ctx();
	BUG_ON(cpu->magic != 0xCAFEBABE);   /* per-CPU area / GS-base corruption tripwire */

	/* Always pass prev to sched_requeue_prev to clear on_cpu */
	cpu->pending_requeue = prev;

	if (prev->state == PROC_RUNNING) {
		prev->state = PROC_READY;
	}

	process_t *next = runqueue_pop();
	/* Never run a process that has already exited. A dying process can be
	 * left in a runqueue (e.g. requeued READY in the return path, then it
	 * takes a fatal signal and calls task_exit before being descheduled).
	 * Running it would resume task_exit's arch_yield, making task_exit
	 * "return" and the fatal signal re-fire — an unkillable livelock. */
	while (next && (next->state == PROC_DEAD || next->state == PROC_ZOMBIE)) {
		next->on_cpu = 0;
		next = runqueue_pop();
	}
	/* cgroup.freeze: requeue a frozen process and try the next, bounded by the
	 * runqueue length so an all-frozen runqueue falls through to idle. */
	if (next && next->pid != 0) {
		int budget = cpu_rq_len(cpu) + 1;
		while (next && next->pid != 0 && cgroup_frozen(next->cgroup) && budget-- > 0) {
			next->on_cpu = 0;
			cpu_rq_add(cpu, next);
			next = runqueue_pop();
			while (next && (next->state == PROC_DEAD || next->state == PROC_ZOMBIE)) {
				next->on_cpu = 0;
				next = runqueue_pop();
			}
		}
		if (next && next->pid != 0 && cgroup_frozen(next->cgroup)) {
			next->on_cpu = 0;
			cpu_rq_add(cpu, next);
			next = NULL;
		}
	}
	if (!next) {
		cpu_context_t *_cpu = arch_get_cpu_ctx();
		next = _cpu->idle ? _cpu->idle : &idle_proc;
	}
	if ((uint64_t)next < 0xFFFF000000000000ULL) {
		kprintf("[DBG] schedule: bad next=%p prev=%p cpu%d rq_head=%p rq_tail=%p\n",
		        next, prev, (int)arch_get_cpu_ctx()->cpu_id,
		        arch_get_cpu_ctx()->rq_head, arch_get_cpu_ctx()->rq_tail);
		kpanic("schedule: invalid next process");
	}
	next->state = PROC_RUNNING;
	next->on_cpu = 1;
	/* nice −20 → 2×quantum, nice +19 → ~half quantum */
	int q = sched_quantum_val - (next->nice * sched_quantum_val) / 40;
	/* cgroup cpu.weight scales the slice (100 = baseline) */
	q = (int)((long)q * cgroup_cpu_weight(next->cgroup) / 100);
	if (q < 1) q = 1;
	next->ticks_remaining = q;

	arch_save_tls(prev);

	if (next->pid != 0 && next->kstack &&
	    *(uint64_t *)next->kstack != KSTACK_CANARY)
		kpanic("kstack overflow detected");

	/*
	 * When switching away from a dead process: defer kstack freeing until
	 * after the stack switch (mov sp,x0), then mark it PROC_ZOMBIE so
	 * proc_reap knows schedule() has completed and process_t is safe to free.
	 * Wake the parent again — it may have found PROC_DEAD too early.
	 */
	if (prev->state == PROC_DEAD && prev->pid != 0 && prev->kstack) {
		cpu->dying_kstack       = prev->kstack;
		cpu->dying_kstack_order = kstack_order_val;
		prev->kstack = NULL;
		prev->state  = PROC_ZOMBIE;
		if (prev->ppid > 0 && (int)prev->ppid < proc_cnt) {
			process_t *pp = proctab[prev->ppid];
			if (pp && pp->state == PROC_SLEEPING) {
				pp->state = PROC_READY;
				if (!pp->on_cpu) runqueue_add(pp);
			}
		}
	}

	cpu->current = next;
	/* IMPORTANT: update the syscall kstack pointer */
	cpu->kstack_top = next->kstack + kstack_size_val;
	smp_mb();

	arch_restore_tls(next);

	spin_unlock_irq(&sched_lock, f);
}

void yield(void)      { arch_yield(); }
void task_sleep(void) {
	uint64_t f = spin_lock_irq(&sched_lock);
	process_t *c = task_current();
	c->sleeping_on_queue = 0;   /* a bare sleep relies on the per-tick poll */
	c->state = PROC_SLEEPING;
	spin_unlock_irq(&sched_lock, f);
	arch_yield();
}

/* Sleep until explicitly woken (task_wake) or a signal arrives (task_signal
 * wakes a sleeping target). sleeping_on_queue=1 → the per-tick blanket wake
 * leaves it alone; the caller must loop on its own condition. */
void task_sleep_interruptible(void) {
	uint64_t f = spin_lock_irq(&sched_lock);
	process_t *c = task_current();
	c->sleeping_on_queue = 1;
	c->state = PROC_SLEEPING;
	spin_unlock_irq(&sched_lock, f);
	arch_yield();
}

/* Mark SLEEPING (sleeping_on_queue=1) then drop `lock`, atomically against a
 * waker holding the same lock. The caller has published its wakeup handle (a
 * waiter field) under `lock`; the waker wakes it with task_wake. IRQs stay
 * masked between the two unlocks, so the registration can't be preempted. */
void task_sleep_unlock(spinlock_t *lock, uint64_t lock_flags) {
	process_t *c = task_current();
	uint64_t sf = spin_lock_irq(&sched_lock);
	c->sleeping_on_queue = 1;
	c->state = PROC_SLEEPING;
	spin_unlock_irq(&sched_lock, sf);
	spin_unlock_irq(lock, lock_flags);
	arch_yield();
}

/* Sleep until tick `deadline` (or a signal). Woken by the deadline sweep in
 * sched_tick, not the per-tick blanket wake — no churn while sleeping. */
void task_sleep_until(uint64_t deadline) {
	uint64_t f = spin_lock_irq(&sched_lock);
	process_t *c = task_current();
	c->wake_deadline      = deadline;
	c->sleeping_on_queue  = 1;   /* deadline sweep / signal wakes us */
	c->state              = PROC_SLEEPING;
	sched_note_wake_deadline(deadline);
	spin_unlock_irq(&sched_lock, f);
	arch_yield();
	c->wake_deadline = 0;
}

/* task_exit -> kernel/proc.c */

void task_wake(process_t *p)
{
	uint64_t f = spin_lock_irq(&sched_lock);
	if (p && p->state == PROC_SLEEPING) {
		p->state = PROC_READY;
		p->sleeping_on_queue = 0;
		if (!p->on_cpu) runqueue_add(p);
		arch_get_cpu_ctx()->need_resched = 1;
	}
	spin_unlock_irq(&sched_lock, f);
}

/* cgroup OOM killer: SIGKILL the largest-footprint task in over_cg's subtree.
 * Lives here for direct access to proctab / runqueue under one sched_lock. The
 * kill is lock-free (set SIGKILL in sig_pending, inline-wake if sleeping) rather
 * than via task_signal — that avoids the lock-ordering and cross-CPU IPI paths
 * that deadlocked when invoked from the memory-charge path. Caller must hold no
 * other lock (this takes only sched_lock, never under vm fault_lock). */
void cgroup_oom_kill(void *over_cg)
{
	/* The task that just tried to grow past memory.max is, by construction, the
	 * one over the limit — kill it (Linux's behaviour for a memcg charge that
	 * can't be satisfied). SIGKILL is set directly in sig_pending rather than via
	 * task_signal: it's the current task, so it dies on the way out of this
	 * syscall, and we avoid the proctab walk / cross-CPU wake that deadlocked. */
	(void)over_cg;
	process_t *cur = task_current();
	if (!cur || cur->pid <= 1) return;
	uint64_t f = spin_lock_irq(&sched_lock);
	cur->sig_pending |= (1UL << (SIGKILL - 1));
	spin_unlock_irq(&sched_lock, f);
}

void task_wake_no_resched(process_t *p)
{
	uint64_t f = spin_lock_irq(&sched_lock);
	if (p && p->state == PROC_SLEEPING) {
		p->state = PROC_READY;
		p->sleeping_on_queue = 0;
		if (!p->on_cpu) runqueue_add(p);
	}
	spin_unlock_irq(&sched_lock, f);
}

/* signal delivery (task_signal, signal_dispatch, …) → kernel/signal.c */

arch_context_t *sched_get_current_ctx(void)
{
	return task_current()->ctx;
}

process_t *task_current(void)
{
	return arch_get_cpu_ctx()->current;
}

int        task_count(void)   { return proc_cnt; }

process_t *task_get(int i)
{
	uint64_t f = spin_lock_irq(&sched_lock);
	process_t *p = (i < 0 || i >= proc_cnt) ? NULL : proctab[i];
	spin_unlock_irq(&sched_lock, f);
	return p;
}

/* zombie reaping / wait helpers (proc_find_zombie/_or_sleep, proc_check_or_sleep, proc_reap) -> kernel/proc.c */

/* Process lifecycle + signal delivery — split out for readability,
 * compiled as part of this file. */
#include "proc.c"
#include "signal.c"
