#ifndef SYNC_H
#define SYNC_H

#include <uapi/types.h>

/*
 * spinlock — busy-waits until lock is free.
 * On single-core this degrades to irq disable/enable;
 * the atomic field is ready for SMP (LL/SC on arm, LOCK CMPXCHG on x86).
 */
/* Ticket spinlock: FIFO-fair. A caller takes a ticket (tail++) and waits until
 * now-serving (head) reaches it, so waiters acquire in arrival order — no
 * starvation under contention, unlike a plain test-and-set. */
typedef struct {
	volatile uint32_t head;    /* now serving */
	volatile uint32_t tail;    /* next ticket */
	volatile int      owner;   /* cpu_id of holder while locked; -1 when free */
} spinlock_t;

#define SPINLOCK_INIT  { 0, 0, -1 }

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

/* atomic_t / atomic64_t built on the RMW primitives above. */
typedef struct { volatile int32_t counter; } atomic_t;
typedef struct { volatile int64_t counter; } atomic64_t;
#define ATOMIC_INIT(i)   { (i) }
#define ATOMIC64_INIT(i) { (i) }

static inline int  atomic_read(const atomic_t *v) { return (int)v->counter; }
static inline void atomic_set(atomic_t *v, int i)  { v->counter = i; }
static inline int  atomic_add_return(atomic_t *v, int i)
{ return (int)atomic_fetch_add32((volatile uint32_t *)&v->counter, (uint32_t)i) + i; }
static inline int  atomic_sub_return(atomic_t *v, int i)
{ return (int)atomic_sub_fetch32((volatile uint32_t *)&v->counter, (uint32_t)i); }
static inline void atomic_inc(atomic_t *v) { atomic_fetch_add32((volatile uint32_t *)&v->counter, 1); }
static inline void atomic_dec(atomic_t *v) { atomic_sub_fetch32((volatile uint32_t *)&v->counter, 1); }
static inline int  atomic_inc_return(atomic_t *v) { return atomic_add_return(v, 1); }
static inline int  atomic_dec_and_test(atomic_t *v)
{ return (int)atomic_sub_fetch32((volatile uint32_t *)&v->counter, 1) == 0; }
static inline int  atomic_cmpxchg(atomic_t *v, int old, int nw)
{ return (int)atomic_cmpxchg32((volatile uint32_t *)&v->counter, (uint32_t)old, (uint32_t)nw); }

static inline int64_t atomic64_read(const atomic64_t *v) { return v->counter; }
static inline void    atomic64_set(atomic64_t *v, int64_t i) { v->counter = i; }
static inline int64_t atomic64_add_return(atomic64_t *v, int64_t i)
{ return (int64_t)atomic_fetch_add64((volatile uint64_t *)&v->counter, (uint64_t)i) + i; }
static inline int64_t atomic64_inc_return(atomic64_t *v) { return atomic64_add_return(v, 1); }
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
	void         *owner_at; /* where it was taken: to name a stuck holder */
} mutex_t;

#define MUTEX_INIT  { SPINLOCK_INIT, 0, 0 }

void mutex_lock(mutex_t *m);
/* Ranix kill-always-wins: like mutex_lock, but gives up with -EINTR (-4) once the
 * caller has a fatal signal (SIGKILL, group exit). Use on every syscall path that
 * may wait behind another task's long hold, so a stuck holder never makes its
 * waiters unkillable too. */
int  mutex_lock_killable(mutex_t *m);
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
/* ... and also wake at tick `deadline` (0: no deadline) */
void sleep_on_locked_until(wait_queue_t *q, spinlock_t *lock, uint64_t irq_flags, uint64_t deadline);
void wake_up(wait_queue_t *q);

/* poll/select/epoll: poll_block() sleeps until poll_notify() or the deadline;
 * producers call poll_notify() when an fd's readiness changes. */
void poll_notify(void);
void poll_arm(void);                 /* call before scanning the fds */
void poll_arm_scan(void);
void poll_notify_key(const void *key);
void poll_note_key(const void *key);
void poll_block(uint64_t deadline);

/* lock diagnostics (sync.c) */
extern volatile int spin_diag_on;
int  spin_held_count(void);
void spin_dump_held(void);
void spin_note_violation(void *lock, void *pc);
void spin_report_bad_unlocks(void);
void atomic_selftest(void);                      /* boot self-test */

extern volatile uint64_t poll_gen;               /* counts readiness changes */

#endif
