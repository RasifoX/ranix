#ifndef SYNC_H
#define SYNC_H

#include <uapi/types.h>

/*
 * spinlock — busy-waits until lock is free.
 * On single-core this degrades to irq disable/enable;
 * the atomic field is ready for SMP (LL/SC on arm, LOCK CMPXCHG on x86).
 */
typedef struct {
	volatile uint32_t locked;
	volatile int      owner;   /* cpu_id of holder while locked; -1 when free */
} spinlock_t;

#define SPINLOCK_INIT  { 0, -1 }

void spin_lock(spinlock_t *l);
void spin_unlock(spinlock_t *l);

/* v8.0-safe atomic RMW for shared refcounts (no LSE dependency). */
uint32_t atomic_fetch_add32(volatile uint32_t *p, uint32_t v);
uint32_t atomic_sub_fetch32(volatile uint32_t *p, uint32_t v);
uint64_t atomic_fetch_add64(volatile uint64_t *p, uint64_t v);
uint64_t atomic_sub_fetch64(volatile uint64_t *p, uint64_t v);
uint32_t atomic_cmpxchg32(volatile uint32_t *p, uint32_t old, uint32_t nw);
uint64_t atomic_cmpxchg64(volatile uint64_t *p, uint64_t old, uint64_t nw);
uint32_t atomic_or32(volatile uint32_t *p, uint32_t v);
uint32_t atomic_and32(volatile uint32_t *p, uint32_t v);
/* IRQ-safe variants — use these from interrupt context */
uint64_t spin_lock_irq(spinlock_t *l);
void     spin_unlock_irq(spinlock_t *l, uint64_t flags);

/*
 * mutex — sleeps instead of spinning.
 * Only usable from task context (not interrupt handlers).
 */
typedef struct {
	spinlock_t    guard;
	volatile int  locked;
	void         *owner;   /* task_t* of current holder, NULL if free */
} mutex_t;

#define MUTEX_INIT  { SPINLOCK_INIT, 0, 0 }

void mutex_lock(mutex_t *m);
void mutex_unlock(mutex_t *m);

/*
 * wait_queue — allows tasks to sleep until a condition is met.
 */
typedef struct wait_queue {
	spinlock_t      lock;
	struct process *tasks;   /* head of task list */
} wait_queue_t;

#define WAIT_QUEUE_INIT  { SPINLOCK_INIT, 0 }

void wait_queue_init(wait_queue_t *q);
void sleep_on(wait_queue_t *q);
/* sleep_on_locked: atomically add to wait queue and release *lock before sleeping.
 * Prevents the lost-wakeup race when the wake condition is checked under *lock. */
void sleep_on_locked(wait_queue_t *q, spinlock_t *lock, uint64_t irq_flags);
void wake_up(wait_queue_t *q);

/* poll/select/epoll: poll_block() sleeps until poll_notify() or the deadline;
 * producers call poll_notify() when an fd's readiness changes. */
void poll_notify(void);
void poll_block(uint64_t deadline);

#endif
