/*
 * POSIX per-process timers (timer_create and friends). Expiry runs in one kernel
 * thread, not the tick: posting a real-time signal allocates. Times are kept on
 * the monotonic clock, so an absolute CLOCK_REALTIME time is converted when set;
 * a later settimeofday does not move an armed timer. Resolution is one tick.
 * With nothing armed the thread sleeps a second, so arming must wake it: the
 * kick flag and the wait queue, both under ptimer_lock, make that wakeup one a
 * thread between its scan and its sleep cannot miss.
 */
#include <kernel/sync.h>

#define PTIMER_MAX    256
#define NS            1000000000ULL
#define SIGEV_SIGNAL_ 0
#define SIGEV_NONE_   1
#define SIGEV_THREAD_ID_ 4
#define SI_TIMER_     (-2)

typedef struct {
	int      used, id;
	pid_t    tgid, tid;       /* tid: SIGEV_THREAD_ID's target, else 0 */
	int      sig, notify, clock;
	uint64_t value;           /* si_value */
	uint64_t next_ns;         /* monotonic; 0 = disarmed */
	uint64_t interval_ns;
	int      overrun;
} ptimer_t;

typedef struct { int64_t sec, nsec; } kts_t;
typedef struct { kts_t interval, value; } kits_t;

static ptimer_t   ptimers[PTIMER_MAX];
static spinlock_t ptimer_lock = SPINLOCK_INIT;
static wait_queue_t ptimer_wq = WAIT_QUEUE_INIT;
static int        ptimer_kick;    /* a timer was armed since the last scan began */
static int        ptimer_started;
static int        ptimer_next_id = 1;

static uint64_t ptimer_now(void)
{
	int64_t s, n;
	mono_time(&s, &n);
	return (uint64_t)s * NS + (uint64_t)n;
}

static void ptimer_thread(void)
{
	for (;;) {
		uint64_t f = spin_lock_irq(&ptimer_lock);
		ptimer_kick = 0;
		spin_unlock_irq(&ptimer_lock, f);
		uint64_t now = ptimer_now();
		int armed = 0;
		for (int i = 0; i < PTIMER_MAX; i++) {
			uint64_t f = spin_lock_irq(&ptimer_lock);
			ptimer_t *t = &ptimers[i];
			ptimer_t c;
			int fire = 0;
			if (t->used && t->next_ns) {
				armed = 1;
				if (t->next_ns <= now) {
					fire = t->notify != SIGEV_NONE_;
					if (t->interval_ns) {
						uint64_t late = (now - t->next_ns) / t->interval_ns;
						t->next_ns += (late + 1) * t->interval_ns;
						t->overrun = late > 0x7fffffff ? 0x7fffffff : (int)late;
					} else {
						t->next_ns = 0;
					}
				}
			}
			c = *t;
			spin_unlock_irq(&ptimer_lock, f);
			if (!fire) continue;
			process_t *p = proc_get(c.tid ? c.tid : c.tgid);
			if (p && p->tgid == c.tgid)
				task_signal_info(p, c.sig, SI_TIMER_, (pid_t)c.id, (uid_t)c.overrun,
				                 c.value, c.tid != 0);
			proc_put(p);
		}
		/* A slot armed after it was scanned left ptimer_kick set: scan again.
		 * Otherwise sleep, released atomically with the lock, so a later arm's
		 * wake_up finds this thread on the queue. */
		f = spin_lock_irq(&ptimer_lock);
		if (ptimer_kick) {
			spin_unlock_irq(&ptimer_lock, f);
			continue;
		}
		sleep_on_locked_until(&ptimer_wq, &ptimer_lock, f,
		                      kernel_ticks + (armed ? 1 : TIMER_HZ));
	}
}

/* exec and the end of a process take their timers with them */
void ptimer_drop(pid_t tgid)
{
	uint64_t f = spin_lock_irq(&ptimer_lock);
	for (int i = 0; i < PTIMER_MAX; i++)
		if (ptimers[i].used && ptimers[i].tgid == tgid) ptimers[i].used = 0;
	spin_unlock_irq(&ptimer_lock, f);
}

static ptimer_t *ptimer_find(int id)
{
	pid_t tgid = task_current()->tgid;
	for (int i = 0; i < PTIMER_MAX; i++)
		if (ptimers[i].used && ptimers[i].id == id && ptimers[i].tgid == tgid) return &ptimers[i];
	return NULL;
}

static long sys_timer_delete(int id);

static long sys_timer_create(int clk, const void *usev, int *uid)
{
	if (clk != CLOCK_REALTIME && clk != CLOCK_MONOTONIC && clk != CLOCK_BOOTTIME) return -EINVAL;
	struct { uint64_t value; int sig, notify, tid; } ev;
	process_t *p = task_current();
	if (usev) {
		if (copy_from_user(&ev, usev, sizeof(ev)) != 0) return -EFAULT;
		if (ev.notify != SIGEV_SIGNAL_ && ev.notify != SIGEV_NONE_ && ev.notify != SIGEV_THREAD_ID_)
			return -EINVAL;
		if (ev.notify != SIGEV_NONE_ && (ev.sig <= 0 || ev.sig > NSIG)) return -EINVAL;
		if (ev.notify == SIGEV_THREAD_ID_) {
			process_t *t = proc_get_user(ev.tid);
			int ok = t && t->tgid == p->tgid;
			pid_t global = t ? (pid_t)t->pid : 0;
			proc_put(t);
			if (!ok) return -EINVAL;
			ev.tid = global;
		}
	}
	{
		uint64_t f = spin_lock_irq(&ptimer_lock);
		int start = !ptimer_started;
		ptimer_started = 1;
		spin_unlock_irq(&ptimer_lock, f);
		if (start) proc_create("ptimer", ptimer_thread);
	}
	uint64_t f = spin_lock_irq(&ptimer_lock);
	ptimer_t *t = NULL;
	for (int i = 0; i < PTIMER_MAX && !t; i++)
		if (!ptimers[i].used) t = &ptimers[i];
	if (!t) { spin_unlock_irq(&ptimer_lock, f); return -EAGAIN; }
	memset(t, 0, sizeof(*t));
	t->used = 1;
	t->id = ptimer_next_id++;
	if (ptimer_next_id < 0) ptimer_next_id = 1;
	t->tgid = p->tgid;
	t->clock = clk;
	t->notify = usev ? ev.notify : SIGEV_SIGNAL_;
	t->sig = usev ? ev.sig : 14 /* SIGALRM */;
	t->value = usev ? ev.value : (uint64_t)t->id;
	t->tid = (usev && ev.notify == SIGEV_THREAD_ID_) ? ev.tid : 0;
	int id = t->id;
	spin_unlock_irq(&ptimer_lock, f);
	if (copy_to_user(uid, &id, sizeof(id)) != 0) { sys_timer_delete(id); return -EFAULT; }
	return 0;
}

static void ptimer_read(ptimer_t *t, uint64_t now, kits_t *o)
{
	uint64_t left = t->next_ns > now ? t->next_ns - now : (t->next_ns ? 1 : 0);
	o->interval.sec = (int64_t)(t->interval_ns / NS);
	o->interval.nsec = (int64_t)(t->interval_ns % NS);
	o->value.sec = (int64_t)(left / NS);
	o->value.nsec = (int64_t)(left % NS);
}

static long sys_timer_settime(int id, int flags, const kits_t *unew, kits_t *uold)
{
	kits_t n, o;
	if (copy_from_user(&n, unew, sizeof(n)) != 0) return -EFAULT;
	if (n.value.sec < 0 || n.value.nsec < 0 || n.value.nsec >= (int64_t)NS ||
	    n.interval.sec < 0 || n.interval.nsec < 0 || n.interval.nsec >= (int64_t)NS) return -EINVAL;
	uint64_t now = ptimer_now();
	uint64_t val = (uint64_t)n.value.sec * NS + (uint64_t)n.value.nsec;
	uint64_t f = spin_lock_irq(&ptimer_lock);
	ptimer_t *t = ptimer_find(id);
	if (!t) { spin_unlock_irq(&ptimer_lock, f); return -EINVAL; }
	ptimer_read(t, now, &o);
	if (val && (flags & 1 /* TIMER_ABSTIME */)) {
		uint64_t base = now;
		if (t->clock == CLOCK_REALTIME) {
			int64_t s, ns;
			real_time(&s, &ns);
			base = (uint64_t)s * NS + (uint64_t)ns;
		}
		val = val > base ? val - base : 1;
	}
	t->next_ns = val ? now + val : 0;
	t->interval_ns = val ? (uint64_t)n.interval.sec * NS + (uint64_t)n.interval.nsec : 0;
	t->overrun = 0;
	int kick = t->next_ns != 0;
	if (kick) ptimer_kick = 1;
	spin_unlock_irq(&ptimer_lock, f);
	if (kick) wake_up(&ptimer_wq);        /* idle, it sleeps for a second */
	if (uold && copy_to_user(uold, &o, sizeof(o)) != 0) return -EFAULT;
	return 0;
}

static long sys_timer_gettime(int id, kits_t *ucur)
{
	kits_t o;
	uint64_t now = ptimer_now();
	uint64_t f = spin_lock_irq(&ptimer_lock);
	ptimer_t *t = ptimer_find(id);
	if (!t) { spin_unlock_irq(&ptimer_lock, f); return -EINVAL; }
	ptimer_read(t, now, &o);
	spin_unlock_irq(&ptimer_lock, f);
	return copy_to_user(ucur, &o, sizeof(o)) == 0 ? 0 : -14;
}

static long sys_timer_getoverrun(int id)
{
	uint64_t f = spin_lock_irq(&ptimer_lock);
	ptimer_t *t = ptimer_find(id);
	long r = t ? t->overrun : -22;
	spin_unlock_irq(&ptimer_lock, f);
	return r;
}

static long sys_timer_delete(int id)
{
	uint64_t f = spin_lock_irq(&ptimer_lock);
	ptimer_t *t = ptimer_find(id);
	if (t) t->used = 0;
	spin_unlock_irq(&ptimer_lock, f);
	return t ? 0 : -22;
}
