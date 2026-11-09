#include <kernel/board.h>
#include <kernel/sync.h>
#include <kernel/irq.h>
#include <kernel/sched.h>
#include <kernel/hal.h>
#include <kernel/kprintf.h>
#include <kernel/ksyms.h>
#include <kernel/smp.h>
#include <kernel/time.h>
#include <kernel/proc.h>
#include <kernel/errno.h>

/* v8.0-safe atomic RMW (cortex-a72 has no LSE, so we cannot let the compiler
 * emit ldadd/etc via __atomic — it would be an undefined instruction). x86
 * uses LOCK xadd. Used for shared refcounts (vm/fd_table) under SMP. */
uint32_t atomic_fetch_add32(volatile uint32_t *p, uint32_t v)
{
#if defined(__aarch64__)
	uint32_t old, nw, res;
	__asm__ volatile(
		"1: ldaxr %w0, [%3]\n"
		"   add   %w1, %w0, %w4\n"
		"   stlxr %w2, %w1, [%3]\n"
		"   cbnz  %w2, 1b\n"
		: "=&r"(old), "=&r"(nw), "=&r"(res)
		: "r"(p), "r"(v) : "memory");
	return old;
#elif defined(__x86_64__)
	__asm__ volatile("lock xaddl %0, %1" : "+r"(v), "+m"(*p) :: "memory");
	return v;
#else
	uint32_t old = *p; *p = old + v; return old;
#endif
}

uint32_t atomic_sub_fetch32(volatile uint32_t *p, uint32_t v)
{
#if defined(__aarch64__)
	uint32_t nw, res;
	__asm__ volatile(
		"1: ldaxr %w0, [%2]\n"
		"   sub   %w0, %w0, %w3\n"
		"   stlxr %w1, %w0, [%2]\n"
		"   cbnz  %w1, 1b\n"
		: "=&r"(nw), "=&r"(res)
		: "r"(p), "r"(v) : "memory");
	return nw;
#elif defined(__x86_64__)
	uint32_t neg = (uint32_t)(-(int32_t)v);
	__asm__ volatile("lock xaddl %0, %1" : "+r"(neg), "+m"(*p) :: "memory");
	return neg - v;
#else
	*p -= v; return *p;
#endif
}

uint64_t atomic_fetch_add64(volatile uint64_t *p, uint64_t v)
{
#if defined(__aarch64__)
	uint64_t old, nw; uint32_t res;
	__asm__ volatile(
		"1: ldaxr %0, [%3]\n"
		"   add   %1, %0, %4\n"
		"   stlxr %w2, %1, [%3]\n"
		"   cbnz  %w2, 1b\n"
		: "=&r"(old), "=&r"(nw), "=&r"(res)
		: "r"(p), "r"(v) : "memory");
	return old;
#elif defined(__x86_64__)
	__asm__ volatile("lock xaddq %0, %1" : "+r"(v), "+m"(*p) :: "memory");
	return v;
#else
	uint64_t old = *p; *p = old + v; return old;
#endif
}

uint64_t atomic_sub_fetch64(volatile uint64_t *p, uint64_t v)
{
#if defined(__aarch64__)
	uint64_t nw; uint32_t res;
	__asm__ volatile(
		"1: ldaxr %0, [%2]\n"
		"   sub   %0, %0, %3\n"
		"   stlxr %w1, %0, [%2]\n"
		"   cbnz  %w1, 1b\n"
		: "=&r"(nw), "=&r"(res) : "r"(p), "r"(v) : "memory");
	return nw;
#elif defined(__x86_64__)
	uint64_t neg = (uint64_t)(-(int64_t)v);
	__asm__ volatile("lock xaddq %0, %1" : "+r"(neg), "+m"(*p) :: "memory");
	return neg - v;
#else
	*p -= v; return *p;
#endif
}

/* Compare-and-swap: if *p == old, store new. Returns the value *p held before. */
uint32_t atomic_cmpxchg32(volatile uint32_t *p, uint32_t old, uint32_t nw)
{
#if defined(__aarch64__)
	uint32_t prev; uint32_t res;
	__asm__ volatile(
		"1: ldaxr %w0, [%2]\n"
		"   cmp   %w0, %w3\n"
		"   b.ne  2f\n"
		"   stlxr %w1, %w4, [%2]\n"
		"   cbnz  %w1, 1b\n"
		"2:\n"
		: "=&r"(prev), "=&r"(res) : "r"(p), "r"(old), "r"(nw) : "memory", "cc");
	return prev;
#elif defined(__x86_64__)
	uint32_t prev;
	__asm__ volatile("lock cmpxchgl %2, %1"
		: "=a"(prev), "+m"(*p) : "r"(nw), "0"(old) : "memory");
	return prev;
#else
	uint32_t prev = *p; if (prev == old) *p = nw; return prev;
#endif
}

uint64_t atomic_cmpxchg64(volatile uint64_t *p, uint64_t old, uint64_t nw)
{
#if defined(__aarch64__)
	uint64_t prev; uint32_t res;
	__asm__ volatile(
		"1: ldaxr %0, [%2]\n"
		"   cmp   %0, %3\n"
		"   b.ne  2f\n"
		"   stlxr %w1, %4, [%2]\n"
		"   cbnz  %w1, 1b\n"
		"2:\n"
		: "=&r"(prev), "=&r"(res) : "r"(p), "r"(old), "r"(nw) : "memory", "cc");
	return prev;
#elif defined(__x86_64__)
	uint64_t prev;
	__asm__ volatile("lock cmpxchgq %2, %1"
		: "=a"(prev), "+m"(*p) : "r"(nw), "0"(old) : "memory");
	return prev;
#else
	uint64_t prev = *p; if (prev == old) *p = nw; return prev;
#endif
}

/* Atomic bit-set / bit-clear on a 32-bit word (returns the previous value). */
uint32_t atomic_or32(volatile uint32_t *p, uint32_t v)
{
#if defined(__aarch64__)
	uint32_t old, nw, res;
	__asm__ volatile(
		"1: ldaxr %w0, [%3]\n"
		"   orr   %w1, %w0, %w4\n"
		"   stlxr %w2, %w1, [%3]\n"
		"   cbnz  %w2, 1b\n"
		: "=&r"(old), "=&r"(nw), "=&r"(res) : "r"(p), "r"(v) : "memory");
	return old;
#elif defined(__x86_64__)
	uint32_t old, prev;
	do { old = *p; prev = atomic_cmpxchg32(p, old, old | v); } while (prev != old);
	return old;
#else
	uint32_t old = *p; *p = old | v; return old;
#endif
}

uint32_t atomic_and32(volatile uint32_t *p, uint32_t v)
{
#if defined(__aarch64__)
	uint32_t old, nw, res;
	__asm__ volatile(
		"1: ldaxr %w0, [%3]\n"
		"   and   %w1, %w0, %w4\n"
		"   stlxr %w2, %w1, [%3]\n"
		"   cbnz  %w2, 1b\n"
		: "=&r"(old), "=&r"(nw), "=&r"(res) : "r"(p), "r"(v) : "memory");
	return old;
#elif defined(__x86_64__)
	uint32_t old, prev;
	do { old = *p; prev = atomic_cmpxchg32(p, old, old & v); } while (prev != old);
	return old;
#else
	uint32_t old = *p; *p = old & v; return old;
#endif
}

/* Verify the atomic RMW assembly produces correct results (the LL/SC loops and
 * LOCK forms are easy to mis-encode). Panics on mismatch so a broken build can
 * never boot. Concurrency itself is exercised by the SMP cow-race test, which
 * pounds the vm refcount through atomic_fetch_add32/sub32. */
void atomic_selftest(void)
{
	volatile uint64_t q = 0;
	if (atomic_fetch_add64(&q, 5) != 0 || q != 5) kpanic("atomic64 add");
	if (atomic_sub_fetch64(&q, 2) != 3 || q != 3) kpanic("atomic64 sub");

	volatile uint32_t v = 10;
	if (atomic_cmpxchg32(&v, 10, 20) != 10 || v != 20) kpanic("cmpxchg32 hit");
	if (atomic_cmpxchg32(&v, 10, 30) != 20 || v != 20) kpanic("cmpxchg32 miss");

	volatile uint64_t w = 0x1ULL << 40;
	if (atomic_cmpxchg64(&w, 0x1ULL << 40, 0x2ULL << 40) != (0x1ULL << 40) ||
	    w != (0x2ULL << 40)) kpanic("cmpxchg64");

	volatile uint32_t b = 0xF0;
	if (atomic_or32(&b, 0x0F) != 0xF0 || b != 0xFF) kpanic("or32");
	if (atomic_and32(&b, 0xF0) != 0xFF || b != 0xF0) kpanic("and32");

	atomic_t a = ATOMIC_INIT(5);
	if (atomic_read(&a) != 5) kpanic("atomic_t read");
	if (atomic_add_return(&a, 3) != 8 || atomic_read(&a) != 8) kpanic("atomic_t add");
	if (atomic_dec_and_test(&a)) kpanic("atomic_t dec_and_test early");
	atomic_set(&a, 1);
	if (!atomic_dec_and_test(&a)) kpanic("atomic_t dec_and_test");
	atomic64_t a64 = ATOMIC64_INIT(0);
	if (atomic64_inc_return(&a64) != 1) kpanic("atomic64_t inc");

	kprintf(KERN_DEBUG "atomic: selftest ok\n");
}

/* Owner tracking is unsafe until the per-CPU context pointer (tpidr_el1 / gs
 * base) is established in arch_cpu_init; enabled from main right after. */
volatile int spin_diag_on = 0;

/* Per-CPU lock bookkeeping, read only by the deadlock report. A ticket lock
 * hands itself to one specific ticket, so "who is queued with which ticket" is
 * what tells a stuck lock's story: the holder (owner), the CPU whose ticket is
 * being served, or nobody at all (a ticket that will never come). The held list
 * is best effort: an IRQ nesting into an update can blur it, the owner field
 * stays authoritative. */
#define SPIN_HELD_MAX 8
struct spin_cpu {
	spinlock_t *volatile wait;    /* lock this CPU is queued on, NULL if none */
	volatile uint32_t    wait_ticket;
	void *volatile       wait_pc;
	spinlock_t *volatile held[SPIN_HELD_MAX];
	void *volatile       held_pc[SPIN_HELD_MAX];
	volatile int         nheld;   /* may exceed SPIN_HELD_MAX; extras untracked */
};
static struct spin_cpu spin_cpus[MAX_CPUS];

/* Unlocks by a CPU that was not the owner. kprintf may take the very lock being
 * released, so they are only recorded here and printed later from the watchdog
 * sweep (spin_report_bad_unlocks) or by the deadlock report. */
#define SPIN_BAD_MAX 8
static struct { spinlock_t *lock; void *pc; int cpu; int owner; } spin_bad[SPIN_BAD_MAX];
static volatile uint32_t spin_bad_n, spin_bad_shown;
volatile uint32_t spin_bad_unlocks;   /* total, for tests via /proc or the log */

/* Under a hypervisor the vCPU holding (or next in line for) the lock can be off
 * the host CPU for a long time; no critical section here lasts anywhere near
 * this, so crossing it means the lock is really lost. */
#define SPIN_STUCK_NS   10000000000ULL
#define SPIN_STUCK_MAX  (1ULL << 36)   /* backstop if the clock does not move */


static void spin_print_pc(const char *pre, void *pc)
{
	uint64_t off = 0;
	const char *s = ksym_lookup((uint64_t)pc, &off);
	if (s) kprintf("%s%s+0x%lx", pre, s, (unsigned long)off);
	else   kprintf("%s%p", pre, pc);
}

static void spin_print_bad(uint32_t from)
{
	uint32_t n = spin_bad_n;
	for (uint32_t i = from; i < n && i < SPIN_BAD_MAX; i++) {
		if (spin_bad[i].owner == -2)
			kprintf(KERN_ERR "smp: cpu%d unlocked IRQs while holding a spinlock (%p)",
			        spin_bad[i].cpu, (void *)spin_bad[i].lock);
		else
			kprintf(KERN_ERR "smp: cpu%d unlocked %p held by cpu%d",
			        spin_bad[i].cpu, (void *)spin_bad[i].lock, spin_bad[i].owner);
		spin_print_pc(" at ", spin_bad[i].pc);
		kprintf("\n");
	}
}

/* Spinlocks this CPU holds right now (best effort, see struct spin_cpu). */
/* What this CPU holds and where each was taken: for a report about the holder. */
void spin_dump_held(void)
{
	if (!spin_diag_on) return;
	int c = arch_get_cpu_ctx()->cpu_id;
	struct spin_cpu *sc = &spin_cpus[c];
	for (int i = 0; i < sc->nheld && i < SPIN_HELD_MAX; i++) {
		kprintf(KERN_ERR "  cpu%d holds %p", c, (void *)sc->held[i]);
		spin_print_pc(" taken at ", sc->held_pc[i]);
		kprintf("\n");
	}
}

int spin_held_count(void)
{
	if (!spin_diag_on) return 0;
	return spin_cpus[arch_get_cpu_ctx()->cpu_id].nheld;
}

/* Record a lock-discipline violation found outside spin_unlock (owner -2 marks
 * it in the report as "IRQs enabled while holding a lock"). */
void spin_note_violation(void *lock, void *pc)
{
	uint32_t i = atomic_fetch_add32(&spin_bad_n, 1);
	atomic_fetch_add32(&spin_bad_unlocks, 1);
	if (i < SPIN_BAD_MAX) {
		spin_bad[i].lock = lock; spin_bad[i].pc = pc;
		spin_bad[i].cpu = spin_diag_on ? (int)arch_get_cpu_ctx()->cpu_id : -1;
		spin_bad[i].owner = -2;
	}
}

/* Called from the cpu0 watchdog sweep (IRQ context, no lock held). */
void spin_report_bad_unlocks(void)
{
	uint32_t n = spin_bad_n, shown = spin_bad_shown;
	if (n == shown) return;
	spin_bad_shown = n;
	spin_print_bad(shown);
	if (n > SPIN_BAD_MAX && shown <= SPIN_BAD_MAX)
		kprintf(KERN_ERR "smp: %u bad unlocks, only %d recorded\n", n, SPIN_BAD_MAX);
}

static void __attribute__((noreturn))
spin_deadlock(spinlock_t *l, uint32_t my, int self, void *pc,
              uint64_t ms, const char *why)
{
	uint32_t head = l->head, tail = l->tail;
	kprintf(KERN_ERR "smp: cpu%d stuck %lu ms on lock %p (serving=%u next=%u mine=%u) "
	        "held by cpu%d: %s\n", self, (unsigned long)ms, (void *)l, head, tail, my,
	        l->owner, why);
	spin_print_pc("  waiter at ", pc);
	kprintf(" (where=%s)\n", arch_get_cpu_ctx()->wd_where ?
	        (const char *)arch_get_cpu_ctx()->wd_where : "?");

	int served = -1;
	for (int c = 0; c < smp_ncpus && c < MAX_CPUS; c++) {
		struct spin_cpu *sc = &spin_cpus[c];
		if (sc->wait) {
			kprintf(KERN_ERR "  cpu%d waits %p ticket %u", c, (void *)sc->wait, sc->wait_ticket);
			spin_print_pc(" from ", sc->wait_pc);
			kprintf("\n");
			if (sc->wait == l && sc->wait_ticket == head) served = c;
		}
		int nh = sc->nheld < SPIN_HELD_MAX ? sc->nheld : SPIN_HELD_MAX;
		for (int i = 0; i < nh; i++) {
			kprintf(KERN_ERR "  cpu%d holds %p", c, (void *)sc->held[i]);
			spin_print_pc(" from ", sc->held_pc[i]);
			kprintf("\n");
		}
	}
	if (l->owner >= 0)
		kprintf(KERN_ERR "  lock is held by cpu%d\n", l->owner);
	else if (served >= 0)
		kprintf(KERN_ERR "  ticket %u is cpu%d's, which does not take it\n", head, served);
	else
		kprintf(KERN_ERR "  ticket %u belongs to no queued cpu: lost by an unlock without a lock\n", head);
	spin_print_bad(0);
	kpanic("spinlock deadlock");
}

static void spin_lock_at(spinlock_t *l, void *pc)
{
	uint32_t my = atomic_fetch_add32(&l->tail, 1);   /* take a ticket */
	if (!spin_diag_on) {
		/* Fast path must not touch arch_get_cpu_ctx(): locks are taken very early
		 * in boot before the per-CPU area (x86 GS-base) is set up, and a deref
		 * there triple-faults. owner is only needed for the diag path below. */
		while (l->head != my)
			cpu_relax();
		return;
	}
	int self = (int)arch_get_cpu_ctx()->cpu_id;
	struct spin_cpu *sc = &spin_cpus[self];
	if (l->head != my) {
		/* Queued: whatever runs on this CPU before our turn and takes the same
		 * lock lines up behind our own ticket, and our ticket is then served to
		 * a CPU that is not looking. Catch it here, not seconds later. */
		spinlock_t *ow = sc->wait; uint32_t ot = sc->wait_ticket; void *op = sc->wait_pc;
		if (ow == l)
			spin_deadlock(l, my, self, pc, 0, "nested wait on a lock this cpu is queued for");
		sc->wait_ticket = my; sc->wait_pc = pc; sc->wait = l;
		uint64_t spins = 0, t0 = 0;
		while (l->head != my) {
			if (l->owner == self)
				kpanic("recursive spinlock (self-deadlock)");
			/* pause/yield: lets the host's pause-loop exit hand our time to the
			 * vCPU we are waiting for, and spares the sibling hyperthread. */
#ifndef SPIN_NO_RELAX
			cpu_relax();
#endif
			if ((++spins & 0xFFFF) == 0) {
				uint64_t now = mono_ns();
				if (!t0) t0 = now;
				else if (now - t0 >= SPIN_STUCK_NS || spins >= SPIN_STUCK_MAX)
					spin_deadlock(l, my, self, pc, (now - t0) / 1000000ULL,
					              "no progress");
			}
		}
		sc->wait = ow; sc->wait_ticket = ot; sc->wait_pc = op;
	}
	l->owner = self;
	int n = sc->nheld++;
	if (n >= 0 && n < SPIN_HELD_MAX) { sc->held[n] = l; sc->held_pc[n] = pc; }
}

void spin_lock(spinlock_t *l)
{
	spin_lock_at(l, __builtin_return_address(0));
}

void spin_unlock(spinlock_t *l)
{
	if (spin_diag_on) {
		int self = (int)arch_get_cpu_ctx()->cpu_id;
		struct spin_cpu *sc = &spin_cpus[self];
		int owner = l->owner;
		if (owner != self) {
			uint32_t i = atomic_fetch_add32(&spin_bad_n, 1);
			atomic_fetch_add32(&spin_bad_unlocks, 1);
			if (i < SPIN_BAD_MAX) {
				spin_bad[i].lock = l; spin_bad[i].pc = __builtin_return_address(0);
				spin_bad[i].cpu = self; spin_bad[i].owner = owner;
			}
		} else {
			int nh = sc->nheld < SPIN_HELD_MAX ? sc->nheld : SPIN_HELD_MAX;
			for (int i = nh - 1; i >= 0; i--)
				if (sc->held[i] == l) {
					for (int j = i; j < nh - 1; j++) {
						sc->held[j] = sc->held[j + 1]; sc->held_pc[j] = sc->held_pc[j + 1];
					}
					break;
				}
			if (sc->nheld > 0) sc->nheld--;
		}
	}
	/* Advance now-serving to hand the lock to the next ticket in FIFO order.
	 * atomic_fetch_add32 carries release ordering (stlxr / lock xadd), so the
	 * critical section's writes are visible before the next waiter proceeds. */
	l->owner = -1;
	atomic_fetch_add32(&l->head, 1);
}

uint64_t spin_lock_irq(spinlock_t *l)
{
	uint64_t flags = irq_save();
	spin_lock_at(l, __builtin_return_address(0));
	return flags;
}

void spin_unlock_irq(spinlock_t *l, uint64_t flags)
{
	spin_unlock(l);
	irq_restore(flags);
}

void mutex_lock(mutex_t *m)
{
	uint64_t since = 0;
	int told = 0;
	while (1) {
		uint64_t f = spin_lock_irq(&m->guard);
		if (!m->locked) {
			m->locked = 1;
			m->owner  = task_current();
			m->owner_at = __builtin_return_address(0);
			spin_unlock_irq(&m->guard, f);
			return;
		}
		process_t *o = m->owner;
		void *at = m->owner_at;
		spin_unlock_irq(&m->guard, f);
		if (!since) since = kernel_ticks;
		else if (!told && kernel_ticks - since > 5 * TIMER_HZ) {
			told = 1;           /* a lock held this long is a bug: name its holder */
			kprintf(KERN_ERR "mutex: %p waited 5 s by pid %d at %p; held by pid %d (%s) state %d, taken at %p\n",
			        m, task_current() ? task_current()->pid : -1, __builtin_return_address(0),
			        o ? o->pid : -1, o ? o->name : "?", o ? (int)o->state : -1, at);
		}
		yield();   /* an unlock wakes nobody, so a sleep here could last for good */
	}
}

int mutex_lock_killable(mutex_t *m)
{
	uint64_t since = 0;
	int told = 0;
	while (1) {
		uint64_t f = spin_lock_irq(&m->guard);
		if (!m->locked) {
			m->locked = 1;
			m->owner  = task_current();
			m->owner_at = __builtin_return_address(0);
			spin_unlock_irq(&m->guard, f);
			return 0;
		}
		process_t *o = m->owner;
		spin_unlock_irq(&m->guard, f);
		process_t *me = task_current();
		if (me && me->pid && fatal_signal_pending(me)) return -EINTR;
		if (!since) since = kernel_ticks;
		else if (!told && kernel_ticks - since > 5 * TIMER_HZ) {
			told = 1;
			kprintf(KERN_ERR "mutex: %p waited 5 s (killable) by pid %d; held by pid %d (%s)\n",
			        m, me ? (int)me->pid : -1, o ? (int)o->pid : -1, o ? o->name : "?");
		}
		yield();
	}
}

void mutex_unlock(mutex_t *m)
{
	uint64_t f = spin_lock_irq(&m->guard);
	m->locked = 0;
	m->owner  = 0;
	spin_unlock_irq(&m->guard, f);
}

void wait_queue_init(wait_queue_t *q)
{
	q->lock = (spinlock_t)SPINLOCK_INIT;
	q->tasks = NULL;
}

/* caller holds q->lock; unlink curr if still queued */
static void wq_unlink_locked(wait_queue_t *q, process_t *curr)
{
	for (process_t **pp = &q->tasks; *pp; pp = &(*pp)->wait_next)
		if (*pp == curr) { *pp = curr->wait_next; curr->wait_next = NULL; return; }
}

/* A waiter must remove itself from the queue on every wakeup — not only wake_up
 * but task_signal / the deadline sweep also flip a sleeper to READY, and those
 * leave it linked. A stale node left in q->tasks corrupts a later wake_up walk
 * (it follows a freed/dangling wait_next and drops the real waiters → lost
 * wakeup). Self-unlink after the yield makes the queue correct for any waker. */
void sleep_on(wait_queue_t *q)
{
	process_t *curr = task_current();
	if (!curr || curr->pid == 0) return;

	uint64_t f = spin_lock_irq(&q->lock);
	curr->state = PROC_SLEEPING;
	curr->sleeping_on_queue = 1;
	process_t *t = q->tasks; int found = 0;
	while (t) { if (t == curr) { found = 1; break; } t = t->wait_next; }
	if (!found) { curr->wait_next = q->tasks; q->tasks = curr; }
	spin_unlock_irq(&q->lock, f);

	arch_yield();

	f = spin_lock_irq(&q->lock);
	wq_unlink_locked(q, curr);
	spin_unlock_irq(&q->lock, f);
}

void sleep_on_locked(wait_queue_t *q, spinlock_t *lock, uint64_t irq_flags)
{
	sleep_on_locked_until(q, lock, irq_flags, 0);
}

/* The same, but also woken by the deadline sweep at tick `deadline` (0: none):
 * for waiters whose event may come without a wake_up (a device that is polled). */
void sleep_on_locked_until(wait_queue_t *q, spinlock_t *lock, uint64_t irq_flags, uint64_t deadline)
{
	process_t *curr = task_current();
	if (!curr || curr->pid == 0) {
		spin_unlock_irq(lock, irq_flags);
		return;
	}

	uint64_t f = spin_lock_irq(&q->lock);
	curr->state = PROC_SLEEPING;
	curr->sleeping_on_queue = 1;
	/* A signal posted before this point woke nobody, and most callers look for
	 * one only after sleeping: stay runnable, or SIGKILL waits for an event that
	 * may never come. */
	if (task_has_actionable_signal()) { curr->state = PROC_READY; curr->sleeping_on_queue = 0; }
	process_t *t = q->tasks; int found = 0;
	while (t) { if (t == curr) { found = 1; break; } t = t->wait_next; }
	if (!found) { curr->wait_next = q->tasks; q->tasks = curr; }
	if (deadline) { curr->wake_deadline = deadline; sched_note_wake_deadline(deadline); }
	spin_unlock_irq(&q->lock, f);
	spin_unlock_irq(lock, irq_flags);
	arch_yield();

	if (deadline) curr->wake_deadline = 0;
	f = spin_lock_irq(&q->lock);
	wq_unlink_locked(q, curr);
	spin_unlock_irq(&q->lock, f);
}

void wake_up(wait_queue_t *q)
{
	uint64_t f = spin_lock_irq(&q->lock);
	/* Detach each node and read its wait_next BEFORE waking it: task_wake makes
	 * the task runnable, so the instant it returns the task may resume on another
	 * CPU and re-enqueue itself, overwriting p->wait_next mid-traversal — a read
	 * after task_wake could follow a stale link and drop the remaining waiters
	 * (lost wakeup). q->lock is held across the whole walk, so a woken task's
	 * re-enqueue blocks until the list is fully drained. */
	process_t *p = q->tasks;
	q->tasks = NULL;
	while (p) {
		process_t *next = p->wait_next;
		p->wait_next = NULL;
		task_wake(p);
		p = next;
	}
	spin_unlock_irq(&q->lock, f);
}

/* ── poll/select wait-queue ───────────────────────────────────────────────────
 * poll/select/epoll register on this shared queue and sleep event-driven (off
 * the per-tick blanket wake). A producer that changes any fd's readiness calls
 * poll_notify(); poll_block also arms a short backstop deadline so a producer
 * that isn't wired yet costs latency, never a hang. */
static wait_queue_t poll_wq = { SPINLOCK_INIT };

volatile uint64_t poll_gen;   /* counts readiness changes, for edge-triggered epoll */

/* Unkeyed events wake every poller, as they always have. A keyed one, from a
 * socket, pipe or eventfd, wakes only who watches that object: a browser has
 * hundreds of sleeping threads and each message used to wake them all to rescan. */
#define PK_BUCKETS 256
static volatile uint64_t poll_all_gen;
static volatile uint64_t poll_kgen[PK_BUCKETS];
#define PK_HASH(k) (((uintptr_t)(k) >> 6 ^ (uintptr_t)(k) >> 14) & (PK_BUCKETS - 1))

void poll_notify(void)
{
	__atomic_add_fetch(&poll_gen, 1, __ATOMIC_SEQ_CST);
	__atomic_add_fetch(&poll_all_gen, 1, __ATOMIC_SEQ_CST);
	wake_up(&poll_wq);
}

/* For the fd scans: from here the sleep depends only on what poll_note_key names. */
void poll_arm_scan(void)
{
	poll_arm();
	task_current()->poll_nk = 0;
}

void poll_notify_key(const void *key)
{
	__atomic_add_fetch(&poll_gen, 1, __ATOMIC_SEQ_CST);
	__atomic_add_fetch(&poll_kgen[PK_HASH(key)], 1, __ATOMIC_SEQ_CST);
	uint64_t f = spin_lock_irq(&poll_wq.lock);
	process_t **pp = &poll_wq.tasks;
	while (*pp) {
		process_t *p = *pp;
		int hit = p->poll_nk < 0;
		for (int i = 0; !hit && i < p->poll_nk; i++)
			hit = p->poll_keys[i] == key;
		if (hit) {
			*pp = p->wait_next;
			p->wait_next = NULL;
			task_wake(p);
		} else {
			pp = &p->wait_next;
		}
	}
	spin_unlock_irq(&poll_wq.lock, f);
}

/* While scanning: this task's sleep depends on key. NULL: on everything. */
void poll_note_key(const void *key)
{
	process_t *c = task_current();
	if (c->poll_nk < 0)
		return;
	if (!key || c->poll_nk == POLL_KEYS) {
		c->poll_nk = -1;
		return;
	}
	c->poll_keys[c->poll_nk] = key;
	c->poll_kseen[c->poll_nk++] = __atomic_load_n(&poll_kgen[PK_HASH(key)], __ATOMIC_SEQ_CST);
}

/* Before looking at the fds. A readiness change after this and before the sleep
 * must not be slept through: without it every such race cost the 100 ms backstop,
 * which in a program that talks to itself over sockets (a browser) was most of
 * its waiting. */
void poll_arm(void)
{
	process_t *c = task_current();
	c->poll_nk = -1;
	c->poll_seen = __atomic_load_n(&poll_all_gen, __ATOMIC_SEQ_CST);
	c->poll_seen_any = __atomic_load_n(&poll_gen, __ATOMIC_SEQ_CST);
}

void poll_block(uint64_t deadline)
{
	process_t *c = task_current();
	uint64_t cap = kernel_ticks + TIMER_HZ / 10;  /* backstop re-check */
	uint64_t d   = (deadline < cap) ? deadline : cap;

	uint64_t f = spin_lock_irq(&poll_wq.lock);
	int stale = __atomic_load_n(&poll_all_gen, __ATOMIC_SEQ_CST) != c->poll_seen;
	if (c->poll_nk < 0)
		stale |= __atomic_load_n(&poll_gen, __ATOMIC_SEQ_CST) != c->poll_seen_any;
	for (int i = 0; !stale && i < c->poll_nk; i++)
		stale = __atomic_load_n(&poll_kgen[PK_HASH(c->poll_keys[i])], __ATOMIC_SEQ_CST) != c->poll_kseen[i];
	if (stale) {
		spin_unlock_irq(&poll_wq.lock, f);
		return;
	}
	process_t *t = poll_wq.tasks; int found = 0;
	while (t) { if (t == c) { found = 1; break; } t = t->wait_next; }
	if (!found) { c->wait_next = poll_wq.tasks; poll_wq.tasks = c; }
	c->state = PROC_SLEEPING;
	c->sleeping_on_queue = 1;
	c->wake_deadline = d;
	sched_note_wake_deadline(d);
	spin_unlock_irq(&poll_wq.lock, f);

	arch_yield();

	c->wake_deadline = 0;
	f = spin_lock_irq(&poll_wq.lock);
	for (process_t **pp = &poll_wq.tasks; *pp; pp = &(*pp)->wait_next)
		if (*pp == c) { *pp = c->wait_next; c->wait_next = NULL; break; }
	spin_unlock_irq(&poll_wq.lock, f);
}
