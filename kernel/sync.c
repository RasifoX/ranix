#include <kernel/sync.h>
#include <kernel/irq.h>
#include <kernel/sched.h>
#include <kernel/hal.h>

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

void spin_lock(spinlock_t *l)
{
	while (!arch_spin_trylock(&l->locked))
		;
}

void spin_unlock(spinlock_t *l)
{
	__asm__ volatile("" ::: "memory");
	l->locked = 0;
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
