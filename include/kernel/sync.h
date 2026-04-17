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
} spinlock_t;

#define SPINLOCK_INIT  { 0 }

void spin_lock(spinlock_t *l);
void spin_unlock(spinlock_t *l);
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

#endif
