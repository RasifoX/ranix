#include <kernel/sync.h>
#include <kernel/irq.h>
#include <kernel/sched.h>
#include <kernel/hal.h>
#include <kernel/kprintf.h>

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

	kprintf("atomic: selftest ok\n");
}

/* Owner tracking is unsafe until the per-CPU context pointer (tpidr_el1 / gs
 * base) is established in arch_cpu_init; enabled from main right after. */
volatile int spin_diag_on = 0;

void spin_lock(spinlock_t *l)
{
	uint32_t my = atomic_fetch_add32(&l->tail, 1);   /* take a ticket */
	if (!spin_diag_on) {
		/* Fast path must not touch arch_get_cpu_ctx(): locks are taken very early
		 * in boot before the per-CPU area (x86 GS-base) is set up, and a deref
		 * there triple-faults. owner is only needed for the diag path below. */
		while (l->head != my)
			;
		return;
	}
	int self = arch_get_cpu_ctx()->cpu_id;
	uint64_t spins = 0;
	while (l->head != my) {
		if (l->owner == self)
			kpanic("recursive spinlock (self-deadlock)");
		if (++spins == 0x3000000) {
			kprintf("spin_lock: cpu%d stuck on lock %p (serving=%u mine=%u) held by cpu%d (where=%s)\n",
			        self, (void *)l, l->head, my, l->owner,
			        arch_get_cpu_ctx()->wd_where ? (const char *)arch_get_cpu_ctx()->wd_where : "?");
			kpanic("spinlock deadlock");
		}
	}
	l->owner = self;
}

void spin_unlock(spinlock_t *l)
{
	/* Advance now-serving to hand the lock to the next ticket in FIFO order.
	 * atomic_fetch_add32 carries release ordering (stlxr / lock xadd), so the
	 * critical section's writes are visible before the next waiter proceeds. */
	l->owner = -1;
	atomic_fetch_add32(&l->head, 1);
}

uint64_t spin_lock_irq(spinlock_t *l)
{
	uint64_t flags = irq_save();
	spin_lock(l);
	return flags;
}

void spin_unlock_irq(spinlock_t *l, uint64_t flags)
{
	spin_unlock(l);
	irq_restore(flags);
}

void mutex_lock(mutex_t *m)
{
	while (1) {
		uint64_t f = spin_lock_irq(&m->guard);
		if (!m->locked) {
			m->locked = 1;
			m->owner  = task_current();
			spin_unlock_irq(&m->guard, f);
			return;
		}
		spin_unlock_irq(&m->guard, f);
		poll_block((uint64_t)-1);
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
	process_t *curr = task_current();
	if (!curr || curr->pid == 0) {
		spin_unlock_irq(lock, irq_flags);
		return;
	}

	uint64_t f = spin_lock_irq(&q->lock);
	curr->state = PROC_SLEEPING;
	curr->sleeping_on_queue = 1;
	process_t *t = q->tasks; int found = 0;
	while (t) { if (t == curr) { found = 1; break; } t = t->wait_next; }
	if (!found) { curr->wait_next = q->tasks; q->tasks = curr; }
	spin_unlock_irq(&q->lock, f);
	spin_unlock_irq(lock, irq_flags);
	arch_yield();

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

void poll_notify(void) { wake_up(&poll_wq); }

void poll_block(uint64_t deadline)
{
	extern volatile uint64_t kernel_ticks;
	process_t *c = task_current();
	uint64_t cap = kernel_ticks + 10;            /* backstop re-check */
	uint64_t d   = (deadline < cap) ? deadline : cap;

	uint64_t f = spin_lock_irq(&poll_wq.lock);
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
