/* ── uname ───────────────────────────────────────────────── */

typedef struct {
	char sysname[65];
	char nodename[65];
	char release[65];
	char version[65];
	char machine[65];
	char domainname[65];
} linux_utsname_t;

char ranix_hostname[65] = "ranix";

static long sys_uname(linux_utsname_t *uts)
{
	if (!uts) return -14;
	linux_utsname_t k;
	memset(&k, 0, sizeof(k));
	memcpy(k.sysname,    "Ranix",   6);
	memcpy(k.nodename,   ranix_hostname, sizeof(ranix_hostname));
	memcpy(k.release,    "1.0.0",   6);
	memcpy(k.version,    "#1 SMP",  7);
#if defined(__x86_64__)
	memcpy(k.machine,    "x86_64",  7);
#else
	memcpy(k.machine,    "aarch64", 8);
#endif
	memcpy(k.domainname, "(none)",  7);
	return copy_to_user(uts, &k, sizeof(k));
}

/* ── time syscalls ───────────────────────────────────────── */

typedef struct { int64_t tv_sec; int64_t tv_nsec; } linux_timespec_t;
typedef struct { int64_t tv_sec; int64_t tv_usec; } linux_timeval_t;

/* ── futex ───────────────────────────────────────────────────── */

#define FUTEX_WAIT         0
#define FUTEX_WAKE         1
#define FUTEX_WAKE_OP      5
#define FUTEX_REQUEUE      3
#define FUTEX_PRIVATE_FLAG 128

extern spinlock_t sched_lock;

/* ── futex: event-driven, per-address hash buckets (Linux futex_q model) ──────
 * Waiters are stack-allocated nodes linked into a bucket keyed by
 * (address space, uaddr). FUTEX_WAIT does an atomic value-check + enqueue under
 * futex_lock, then sleeps; FUTEX_WAKE dequeues and wakes — no fixed slot table,
 * no slot-reuse races. The wait loop re-reads the user word each iteration, so a
 * release whose wake was never delivered is still noticed. (A non-timed waiter
 * could set sleeping_on_queue=1 to drop the per-tick poll entirely — phase 2,
 * with the blocking-I/O wait-queue rework; kept 0 here for signal/timeout
 * responsiveness via the existing tick walk.) */
typedef struct futex_q {
	struct process *task;
	uint32_t       *uaddr;
	void           *mm;
	int             woken;
	struct futex_q *next;
} futex_q_t;

#define FUTEX_BUCKETS 64
static futex_q_t  *futex_bucket[FUTEX_BUCKETS];
static spinlock_t  futex_lock = SPINLOCK_INIT;

static unsigned futex_hash(void *mm, uint32_t *uaddr)
{
	uintptr_t a = (uintptr_t)uaddr >> 2, m = (uintptr_t)mm >> 6;
	return (unsigned)((a ^ m) & (FUTEX_BUCKETS - 1));
}

/* caller holds futex_lock; unlink q if still queued, return 1 if found */
static int futex_unlink(futex_q_t *q)
{
	for (futex_q_t **pp = &futex_bucket[futex_hash(q->mm, q->uaddr)]; *pp; pp = &(*pp)->next)
		if (*pp == q) { *pp = q->next; q->next = NULL; return 1; }
	return 0;
}

/* caller holds futex_lock; wake up to max waiters on (mm,uaddr), requeue up to
 * max_req of the remainder onto uaddr2. returns number woken. */
static int futex_do_wake(void *mm, uint32_t *uaddr, int max,
                         uint32_t *uaddr2, int max_req)
{
	int woken = 0, requeued = 0;
	futex_q_t **pp = &futex_bucket[futex_hash(mm, uaddr)];
	while (*pp) {
		futex_q_t *q = *pp;
		if (q->mm != mm || q->uaddr != uaddr) { pp = &q->next; continue; }
		if (woken < max) {
			*pp = q->next; q->next = NULL;
			q->woken = 1;
			task_wake(q->task);
			woken++;
		} else if (uaddr2 && requeued < max_req) {
			*pp = q->next;
			q->uaddr = uaddr2;
			unsigned h2 = futex_hash(mm, uaddr2);
			q->next = futex_bucket[h2]; futex_bucket[h2] = q;
			requeued++;
		} else {
			pp = &q->next;
		}
	}
	return woken;
}

static long sys_futex(uint32_t *uaddr, int op, uint32_t val,
                      linux_timespec_t *timeout, uint32_t *uaddr2, uint32_t val3)
{
	int cmd = op & ~FUTEX_PRIVATE_FLAG;
	process_t *self = task_current();
	void *mm = self->vm;

	if (cmd == FUTEX_WAIT || cmd == 9 /* FUTEX_WAIT_BITSET */) {
		int timed = 0;
		uint64_t end = 0;
		if (timeout) {
			linux_timespec_t kts;
			if (copy_from_user(&kts, timeout, sizeof(kts)) == 0) {
				end = kernel_ticks + (uint64_t)kts.tv_sec * TIMER_HZ +
				      (uint64_t)kts.tv_nsec / (1000000000 / TIMER_HZ);
				timed = 1;
			}
		}
		futex_q_t q = { self, uaddr, mm, 0, NULL };
		uint64_t f = spin_lock_irq(&futex_lock);
		uint32_t cur;
		if (copy_from_user(&cur, uaddr, sizeof(cur)) != 0) { spin_unlock_irq(&futex_lock, f); return -14; }
		if (cur != val)                                    { spin_unlock_irq(&futex_lock, f); return -11; }
		unsigned h = futex_hash(mm, uaddr);
		q.next = futex_bucket[h]; futex_bucket[h] = &q;
		uint64_t fs = spin_lock_irq(&sched_lock);
		self->state = PROC_SLEEPING;
		/* event-driven: woken by futex_do_wake or task_signal, so the per-tick
		 * blanket wake can skip us. timed waits stay on the poll for their
		 * deadline (no per-waiter timer yet). */
		self->sleeping_on_queue = timed ? 0 : 1;
		spin_unlock_irq(&sched_lock, fs);
		spin_unlock_irq(&futex_lock, f);

		for (;;) {
			arch_yield();
			f = spin_lock_irq(&futex_lock);
			if (q.woken) { spin_unlock_irq(&futex_lock, f); return 0; }
			int sig = (self->sig_pending & ~self->sig_mask) != 0;
			uint32_t c2;
			int vchg = (copy_from_user(&c2, uaddr, sizeof(c2)) == 0 && c2 != val);
			int tmo  = timed && kernel_ticks >= end;
			if (sig || vchg || tmo) {
				futex_unlink(&q);
				spin_unlock_irq(&futex_lock, f);
				return sig ? -4 : (tmo ? -110 : -11);
			}
			fs = spin_lock_irq(&sched_lock);
			self->state = PROC_SLEEPING;
			spin_unlock_irq(&sched_lock, fs);
			spin_unlock_irq(&futex_lock, f);
		}
	}

	if (cmd == FUTEX_WAKE) {
		uint64_t f = spin_lock_irq(&futex_lock);
		int woken = futex_do_wake(mm, uaddr, (int)val, NULL, 0);
		spin_unlock_irq(&futex_lock, f);
		return woken;
	}

	if (cmd == FUTEX_WAKE_OP) {
		if (!uaddr2) return -22;
		uint32_t op3 = (val3 >> 28) & 0xf, cmp3 = (val3 >> 24) & 0xf;
		uint32_t oparg = (val3 >> 12) & 0xfff, cmparg = val3 & 0xfff;
		if (op3 & 8) { oparg = 1u << oparg; op3 &= ~8u; }
		uint32_t oldval = 0;
		if (copy_from_user(&oldval, uaddr2, sizeof(oldval)) != 0) return -14;
		uint32_t newval = oldval;
		switch (op3) {
		case 0: newval  = oparg; break; case 1: newval += oparg; break;
		case 2: newval |= oparg; break; case 3: newval &= ~oparg; break;
		case 4: newval ^= oparg; break;
		}
		if (copy_to_user(uaddr2, &newval, sizeof(newval)) != 0) return -14;
		uint64_t f = spin_lock_irq(&futex_lock);
		int woken = futex_do_wake(mm, uaddr, (int)val, NULL, 0);
		int do2 = 0;
		switch (cmp3) {
		case 0: do2 = (oldval == cmparg); break; case 1: do2 = (oldval != cmparg); break;
		case 2: do2 = ((int32_t)oldval <  (int32_t)cmparg); break;
		case 3: do2 = ((int32_t)oldval <= (int32_t)cmparg); break;
		case 4: do2 = ((int32_t)oldval >  (int32_t)cmparg); break;
		case 5: do2 = ((int32_t)oldval >= (int32_t)cmparg); break;
		}
		if (do2) woken += futex_do_wake(mm, uaddr2, (int)(uintptr_t)timeout, NULL, 0);
		spin_unlock_irq(&futex_lock, f);
		return woken;
	}

	if (cmd == FUTEX_REQUEUE || cmd == 4 /* FUTEX_CMP_REQUEUE */) {
		if (cmd == 4) {
			uint64_t fq = spin_lock_irq(&futex_lock);
			uint32_t cur;
			if (copy_from_user(&cur, uaddr, sizeof(cur)) != 0) { spin_unlock_irq(&futex_lock, fq); return -14; }
			if (cur != val3)                                   { spin_unlock_irq(&futex_lock, fq); return -11; }
			spin_unlock_irq(&futex_lock, fq);
		}
		uint64_t f = spin_lock_irq(&futex_lock);
		int woken = futex_do_wake(mm, uaddr, (int)val, uaddr2, (int)(uintptr_t)timeout);
		spin_unlock_irq(&futex_lock, f);
		return woken;
	}

	return -38;
}

/* Called from sched.c task_exit to wake futex waiters on clear_child_tid. */
void futex_wake_addr(uint32_t *uaddr)
{
	if (!uaddr) return;
	process_t *self = task_current();
	uint64_t f = spin_lock_irq(&futex_lock);
	futex_do_wake(self ? self->vm : NULL, uaddr, 0x7fffffff, NULL, 0);
	spin_unlock_irq(&futex_lock, f);
}

/* Monotonic wall time. On aarch64 read the generic counter for ~ns
 * resolution (the 100Hz tick clock is only 10ms); EL1 may always read
 * cntvct_el0. Split-divide to avoid 64-bit overflow on cnt*1e9. */
static void mono_time(int64_t *sec, int64_t *nsec)
{
#if defined(__aarch64__)
	uint64_t cnt, frq;
	__asm__ volatile("mrs %0, cntvct_el0" : "=r"(cnt));
	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frq));
	if (frq) {
		*sec  = (int64_t)(cnt / frq);
		*nsec = (int64_t)(((cnt % frq) * 1000000000ULL) / frq);
		return;
	}
#endif
	*sec  = (int64_t)(kernel_ticks / TIMER_HZ);
	*nsec = (int64_t)((kernel_ticks % TIMER_HZ) * (1000000000 / TIMER_HZ));
}

static long sys_clock_gettime(int clkid, linux_timespec_t *tp)
{
	(void)clkid;
	if (!tp) return -14;
	linux_timespec_t k;
	mono_time(&k.tv_sec, &k.tv_nsec);
	return copy_to_user(tp, &k, sizeof(k));
}

static long sys_gettimeofday(linux_timeval_t *tv, void *tz)
{
	(void)tz;
	if (!tv) return 0;
	linux_timeval_t k;
	int64_t s, ns;
	mono_time(&s, &ns);
	k.tv_sec  = s;
	k.tv_usec = ns / 1000;
	return copy_to_user(tv, &k, sizeof(k));
}

static long sys_nanosleep(linux_timespec_t *req, linux_timespec_t *rem)
{
	(void)rem;
	if (!req) return -14;
	linux_timespec_t k;
	if (copy_from_user(&k, req, sizeof(k)) != 0) return -14;
	uint64_t ticks = (uint64_t)k.tv_sec * TIMER_HZ +
	                 (uint64_t)k.tv_nsec / (1000000000 / TIMER_HZ);
	uint64_t end = kernel_ticks + (ticks ? ticks : 1);
	process_t *cp = task_current();
	while (kernel_ticks < end) {
		if (cp->sig_pending & ~cp->sig_mask) return -4; /* EINTR */
		task_sleep_until(end);
	}
	return 0;
}

typedef struct { int64_t tv_sec; int64_t tv_usec; } itimerval_tv_t;
typedef struct { itimerval_tv_t it_interval; itimerval_tv_t it_value; } itimerval_t;

static long sys_setitimer(int which, const itimerval_t *new_v, itimerval_t *old_v)
{
    if (which != 0 /* ITIMER_REAL */) return 0; /* ignore VIRTUAL/PROF */
    process_t *p = task_current();
    if (old_v) {
        itimerval_t oval;
        uint64_t hz = TIMER_HZ;
        oval.it_value.tv_sec    = (int64_t)(p->itimer_value / hz);
        oval.it_value.tv_usec   = (int64_t)((p->itimer_value % hz) * (1000000 / hz));
        oval.it_interval.tv_sec = (int64_t)(p->itimer_interval / hz);
        oval.it_interval.tv_usec= (int64_t)((p->itimer_interval % hz) * (1000000 / hz));
        copy_to_user(old_v, &oval, sizeof(oval));
    }
    if (new_v) {
        itimerval_t nval;
        if (copy_from_user(&nval, new_v, sizeof(nval)) != 0) return -14;
        uint64_t hz = TIMER_HZ;
        p->itimer_value    = (uint64_t)nval.it_value.tv_sec * hz +
                             (uint64_t)nval.it_value.tv_usec * hz / 1000000;
        p->itimer_interval = (uint64_t)nval.it_interval.tv_sec * hz +
                             (uint64_t)nval.it_interval.tv_usec * hz / 1000000;
    }
    return 0;
}

static long sys_getitimer(int which, itimerval_t *val)
{
    return sys_setitimer(which, NULL, val);
}

static long sys_times(void *buf)
{
	if (buf) {
		int64_t zeros[4] = {0};
		copy_to_user(buf, zeros, sizeof(zeros));
	}
	return (long)kernel_ticks;
}

static long sys_sysinfo(void *info)
{
	if (!info) return 0;
	/* struct sysinfo layout (Linux ABI) */
	struct {
		int64_t  uptime;
		uint64_t loads[3];
		uint64_t totalram;
		uint64_t freeram;
		uint64_t sharedram;
		uint64_t bufferram;
		uint64_t totalswap;
		uint64_t freeswap;
		uint16_t procs;
		uint8_t  _pad[22];
		uint32_t mem_unit;
	} si = {0};
	si.uptime    = (int64_t)(kernel_ticks / TIMER_HZ);
	si.totalram  = buddy_total_pages() * PAGE_SIZE;
	si.freeram   = buddy_free_pages()  * PAGE_SIZE;
	si.procs     = (uint16_t)task_count();
	si.mem_unit  = 1;
	copy_to_user(info, &si, sizeof(si));
	return 0;
}

