#include <kernel/sync.h>
#include <kernel/irq.h>
#include <kernel/sched.h>
#include <kernel/hal.h>
#include <kernel/kprintf.h>

/* arch_spin_trylock: attempt to atomically set locked 0→1.
 * Returns 1 on success, 0 if already locked. */
static int arch_spin_trylock(volatile uint32_t *p)
{
#if defined(__aarch64__)
	uint32_t tmp, res;
	__asm__ volatile(
		"1: ldaxr %w0, [%2]\n"
		"   cbnz  %w0, 2f\n"
		"   stxr  %w1, %w3, [%2]\n"
		"   cbnz  %w1, 1b\n"
		"   mov   %w0, #1\n"
		"   b     3f\n"
		"2: mov   %w0, #0\n"
		"3:\n"
		: "=&r"(tmp), "=&r"(res)
		: "r"(p), "r"(1u)
		: "memory"
	);
	return (int)tmp;
#elif defined(__x86_64__)
	uint32_t old = 0, new = 1;
	__asm__ volatile(
		"lock cmpxchgl %2, %1"
		: "=a"(old), "+m"(*p)
		: "r"(new), "0"(old)
		: "memory"
	);
	return old == 0;
#else
	/* fallback for unknown arch — not SMP safe */
	if (*p) return 0;
	*p = 1;
	return 1;
#endif
}

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

/* Owner tracking is unsafe until the per-CPU context pointer (tpidr_el1 / gs
 * base) is established in arch_cpu_init; enabled from main right after. */
volatile int spin_diag_on = 0;

void spin_lock(spinlock_t *l)
{
	if (!spin_diag_on) {
		while (!arch_spin_trylock(&l->locked))
			;
		return;
	}
	int self = arch_get_cpu_ctx()->cpu_id;
	uint64_t spins = 0;
	while (!arch_spin_trylock(&l->locked)) {
		if (l->owner == self)
			kpanic("recursive spinlock (self-deadlock)");
		if (++spins == 0x3000000) {
			kprintf("spin_lock: cpu%d stuck on lock %p held by cpu%d (where=%s)\n",
			        self, (void *)l, l->owner,
			        arch_get_cpu_ctx()->wd_where ? (const char *)arch_get_cpu_ctx()->wd_where : "?");
			kpanic("spinlock deadlock");
		}
	}
	l->owner = self;
}

void spin_unlock(spinlock_t *l)
{
	l->owner = -1;
#if defined(__aarch64__)
	__asm__ volatile("stlr wzr, [%0]" :: "r"(&l->locked) : "memory");
#else
	__asm__ volatile("" ::: "memory");
	l->locked = 0;
#endif
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
		task_sleep();
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

void sleep_on(wait_queue_t *q)
{
	process_t *curr = task_current();
	if (!curr || curr->pid == 0) return;

	uint64_t f = spin_lock_irq(&q->lock);
	curr->state = PROC_SLEEPING;
	curr->sleeping_on_queue = 1;
	/* Avoid duplicate entries (sched_tick may wake without removing) */
	process_t *t = q->tasks; int found = 0;
	while (t) { if (t == curr) { found = 1; break; } t = t->wait_next; }
	if (!found) { curr->wait_next = q->tasks; q->tasks = curr; }
	spin_unlock_irq(&q->lock, f);

	arch_yield();
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
	/* Avoid duplicate entries (sched_tick may wake without removing) */
	process_t *t = q->tasks; int found = 0;
	while (t) { if (t == curr) { found = 1; break; } t = t->wait_next; }
	if (!found) { curr->wait_next = q->tasks; q->tasks = curr; }
	spin_unlock_irq(&q->lock, f);
	spin_unlock_irq(lock, irq_flags);
	arch_yield();
}

void wake_up(wait_queue_t *q)
{
	uint64_t f = spin_lock_irq(&q->lock);
	process_t *p = q->tasks;
	while (p) {
		task_wake(p);
		p = p->wait_next;
	}
	q->tasks = NULL;
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
	spin_unlock_irq(&poll_wq.lock, f);

	arch_yield();

	c->wake_deadline = 0;
	f = spin_lock_irq(&poll_wq.lock);
	for (process_t **pp = &poll_wq.tasks; *pp; pp = &(*pp)->wait_next)
		if (*pp == c) { *pp = c->wait_next; c->wait_next = NULL; break; }
	spin_unlock_irq(&poll_wq.lock, f);
}
