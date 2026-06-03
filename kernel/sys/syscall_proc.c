/* ── uname ───────────────────────────────────────────────── */

typedef struct {
	char sysname[65];
	char nodename[65];
	char release[65];
	char version[65];
	char machine[65];
	char domainname[65];
} linux_utsname_t;

static long sys_uname(linux_utsname_t *uts)
{
	if (!uts) return -14;
	uts_namespace_t *ns = current_uts();
	linux_utsname_t k;
	memset(&k, 0, sizeof(k));
	memcpy(k.sysname,    "Ranix",   6);
	memcpy(k.nodename,   ns->nodename,   sizeof(k.nodename));
	memcpy(k.release,    "1.0.0",   6);
	memcpy(k.version,    "#1 SMP",  7);
#if defined(__x86_64__)
	memcpy(k.machine,    "x86_64",  7);
#else
	memcpy(k.machine,    "aarch64", 8);
#endif
	memcpy(k.domainname, ns->domainname, sizeof(k.domainname));
	return copy_to_user(uts, &k, sizeof(k));
}

/*
 * unshare(2) — detach the caller from namespaces shared with its parent.
 * Only CLONE_NEWUTS is supported: it gives the caller a private copy of the
 * UTS namespace, so later sethostname/uname changes stay local.
 */
static long sys_unshare(unsigned long flags)
{
	process_t *p = task_current();
	if (flags & CLONE_NEWUTS) {
		uts_namespace_t *ns = uts_ns_clone(p->uts_ns);
		if (!ns) return -12; /* ENOMEM */
		uts_ns_put(p->uts_ns);
		p->uts_ns = ns;
	}
	if (flags & CLONE_NEWIPC) {
		ipc_namespace_t *ns = ipc_ns_clone(p->ipc_ns);
		if (!ns) return -12; /* ENOMEM */
		ipc_ns_put(p->ipc_ns);
		p->ipc_ns = ns;
	}
	if (flags & CLONE_NEWTIME) {
		/* the new time ns applies to future children, not the caller, so its
		 * offsets can still be set via /proc/self/timens_offsets */
		if (!p->time_ns_new) {
			time_namespace_t *ns = time_ns_clone(p->time_ns);
			if (!ns) return -12; /* ENOMEM */
			p->time_ns_new = ns;
		}
	}
	if (flags & CLONE_NEWNS) {
		mnt_namespace_t *ns = mnt_ns_clone(p->mnt_ns);
		if (!ns) return -12; /* ENOMEM */
		mnt_ns_put(p->mnt_ns);
		p->mnt_ns = ns;
	}
	if (flags & CLONE_NEWUSER) {
		/* moves the caller into a new user namespace and makes it all-powerful
		 * there (rootless-container basis); uid_map is written afterwards */
		user_namespace_t *ns = user_ns_create_child(p->user_ns ? p->user_ns : &init_user_ns, p->euid);
		if (!ns) return -12;
		user_ns_put(p->user_ns);
		p->user_ns = ns;
		p->cap_effective = p->cap_permitted = CAP_FULL;
	}
	if (flags & CLONE_NEWNET) {
		/* moves the caller into a new network namespace (only loopback) */
		net_namespace_t *ns = net_ns_create();
		if (!ns) return -12;
		net_ns_put(p->net_ns);
		p->net_ns = ns;
	}
	if (flags & CLONE_NEWCGROUP) {
		/* the caller's current cgroup becomes the root of the new ns's view */
		struct cgroup_namespace *ns = cgroup_ns_create();
		if (!ns) return -12;
		cgroup_ns_put(p->cgroup_ns);
		p->cgroup_ns = ns;
	}
	if (flags & CLONE_NEWPID) {
		/* applies to future children, not the caller (the caller's pid does
		 * not change); the first child becomes nr 1 of the new ns */
		if (!p->pid_ns_new) {
			pid_namespace_t *ns = pid_ns_create_child(p->pid_ns ? p->pid_ns : &init_pid_ns);
			if (!ns) return -12; /* ENOMEM */
			p->pid_ns_new = ns;
		}
	}
	return 0;
}

/*
 * setns(2) — join the namespace referred to by an open ns fd. Only the UTS
 * namespace is supported; nstype, if given, must match.
 */
static long sys_setns(int fd, int nstype)
{
	process_t *p = task_current();
	vfs_file_t *f = fd_get(p->fds, fd);
	if (!f) return -9; /* EBADF */

	uts_namespace_t *uts = nsfile_to_uts(f);
	if (uts) {
		if (nstype != 0 && nstype != CLONE_NEWUTS) return -22;
		uts_ns_get(uts);
		uts_ns_put(p->uts_ns);
		p->uts_ns = uts;
		return 0;
	}

	ipc_namespace_t *ipc = nsfile_to_ipc(f);
	if (ipc) {
		if (nstype != 0 && nstype != CLONE_NEWIPC) return -22;
		ipc_ns_get(ipc);
		ipc_ns_put(p->ipc_ns);
		p->ipc_ns = ipc;
		return 0;
	}

	time_namespace_t *tns = nsfile_to_time(f);
	if (tns) {
		if (nstype != 0 && nstype != CLONE_NEWTIME) return -22;
		time_ns_get(tns);
		time_ns_put(p->time_ns);
		p->time_ns = tns;
		return 0;
	}

	mnt_namespace_t *mns = nsfile_to_mnt(f);
	if (mns) {
		if (nstype != 0 && nstype != CLONE_NEWNS) return -22;
		mnt_ns_get(mns);
		mnt_ns_put(p->mnt_ns);
		p->mnt_ns = mns;
		return 0;
	}

	pid_namespace_t *pns = nsfile_to_pid(f);
	if (pns) {
		if (nstype != 0 && nstype != CLONE_NEWPID) return -22;
		/* like unshare(CLONE_NEWPID): the caller's own pid ns is unchanged;
		 * the joined ns becomes the ns for its future children */
		pid_ns_get(pns);
		if (p->pid_ns_new) pid_ns_put(p->pid_ns_new);
		p->pid_ns_new = pns;
		return 0;
	}

	user_namespace_t *uns = nsfile_to_user(f);
	if (uns) {
		if (nstype != 0 && nstype != CLONE_NEWUSER) return -22;
		user_ns_get(uns);
		user_ns_put(p->user_ns);
		p->user_ns = uns;
		return 0;
	}

	net_namespace_t *nns = nsfile_to_net(f);
	if (nns) {
		if (nstype != 0 && nstype != CLONE_NEWNET) return -22;
		net_ns_get(nns);
		net_ns_put(p->net_ns);
		p->net_ns = nns;
		return 0;
	}

	struct cgroup_namespace *cns = nsfile_to_cgroup(f);
	if (cns) {
		if (nstype != 0 && nstype != CLONE_NEWCGROUP) return -22;
		cgroup_ns_get(cns);
		cgroup_ns_put(p->cgroup_ns);
		p->cgroup_ns = cns;
		return 0;
	}

	return -22; /* EINVAL: not a namespace fd */
}

/* ── time syscalls ───────────────────────────────────────── */

typedef struct { int64_t tv_sec; int64_t tv_nsec; } linux_timespec_t;
typedef struct { int64_t tv_sec; int64_t tv_usec; } linux_timeval_t;

/* ── futex ───────────────────────────────────────────────────── */

#define FUTEX_WAIT         0
#define FUTEX_WAKE         1
#define FUTEX_WAKE_OP      5
#define FUTEX_REQUEUE      3
#define FUTEX_PRIVATE_FLAG    128
#define FUTEX_CLOCK_REALTIME  256
#define FUTEX_CMD_MASK        (~(FUTEX_PRIVATE_FLAG | FUTEX_CLOCK_REALTIME))

extern spinlock_t sched_lock;

/* ── futex: event-driven, per-address hash buckets (futex_q model) ──────
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

static void mono_time(int64_t *sec, int64_t *nsec);

/* Tick value for the deadline sweep to wake a timed futex waiter at the ns
 * deadline. Recomputed each time the waiter sleeps so a fast kernel_ticks under
 * TCG cannot leave it spinning — the loop re-checks the real deadline anyway. */
static uint64_t futex_deadline_ticks(int64_t end_ns)
{
	int64_t s, n; mono_time(&s, &n);
	int64_t rem = end_ns - (s * 1000000000LL + n);
	if (rem < 0) rem = 0;
	return kernel_ticks + (uint64_t)rem / (1000000000 / TIMER_HZ) + 1;
}

static long sys_futex(uint32_t *uaddr, int op, uint32_t val,
                      linux_timespec_t *timeout, uint32_t *uaddr2, uint32_t val3)
{
	int cmd = op & FUTEX_CMD_MASK;
	process_t *self = task_current();
	void *mm = self->vm;

	if (cmd == FUTEX_WAIT || cmd == 9 /* FUTEX_WAIT_BITSET */) {
		int timed = 0;
		int64_t end_ns = 0;   /* absolute mono_time deadline (ns) */
		if (timeout) {
			linux_timespec_t kts;
			if (copy_from_user(&kts, timeout, sizeof(kts)) == 0) {
				/* FUTEX_WAIT's timeout is a relative span, FUTEX_WAIT_BITSET's an
				 * absolute deadline on the same boot-relative clock mono_time
				 * reports. Track the deadline in ns and decide the timeout against
				 * mono_time — kernel_ticks does not track wall time under TCG, so
				 * it serves only as the deadline-sweep wake-up hint below. */
				int64_t ns, nn; mono_time(&ns, &nn);
				int64_t now_ns = ns * 1000000000LL + nn;
				int64_t want   = kts.tv_sec * 1000000000LL + kts.tv_nsec;
				end_ns = (cmd == 9) ? want : now_ns + want;
				timed  = 1;
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
		/* event-driven: woken by futex_do_wake or task_signal. timed waits also
		 * arm wake_deadline so the sched_tick deadline sweep fires a wake-up — no
		 * per-tick blanket poll either way. */
		self->sleeping_on_queue = 1;
		if (timed) self->wake_deadline = futex_deadline_ticks(end_ns);
		spin_unlock_irq(&sched_lock, fs);
		spin_unlock_irq(&futex_lock, f);

		for (;;) {
			arch_yield();
			f = spin_lock_irq(&futex_lock);
			if (q.woken) { self->wake_deadline = 0; spin_unlock_irq(&futex_lock, f); return 0; }
			int sig = (self->sig_pending & ~self->sig_mask) != 0;
			uint32_t c2;
			int vchg = (copy_from_user(&c2, uaddr, sizeof(c2)) == 0 && c2 != val);
			int tmo  = 0;
			if (timed) { int64_t s, n; mono_time(&s, &n); tmo = (s * 1000000000LL + n) >= end_ns; }
			if (sig || vchg || tmo) {
				self->wake_deadline = 0;
				futex_unlink(&q);
				spin_unlock_irq(&futex_lock, f);
				return sig ? -4 : (tmo ? -110 : -11);
			}
			fs = spin_lock_irq(&sched_lock);
			self->state = PROC_SLEEPING;
			self->sleeping_on_queue = 1;
			if (timed) self->wake_deadline = futex_deadline_ticks(end_ns);
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
#elif defined(__x86_64__)
	extern uint64_t hpet_now_ns(void);
	uint64_t hns = hpet_now_ns();
	if (hns) {
		*sec  = (int64_t)(hns / 1000000000ULL);
		*nsec = (int64_t)(hns % 1000000000ULL);
		return;
	}
#endif
	*sec  = (int64_t)(kernel_ticks / TIMER_HZ);
	*nsec = (int64_t)((kernel_ticks % TIMER_HZ) * (1000000000 / TIMER_HZ));
}

#define CLOCK_REALTIME   0
#define CLOCK_MONOTONIC  1
#define CLOCK_BOOTTIME   7

/* realtime = mono_time + g_rt_off (set from the RTC at boot, or settimeofday/
 * clock_settime / NTP later). 0 offset until rtc_init → falls back to uptime. */
static int64_t g_rt_off_sec  = 0;
static int64_t g_rt_off_nsec = 0;

static void real_time(int64_t *sec, int64_t *nsec)
{
	int64_t ms, mn; mono_time(&ms, &mn);
	int64_t s = ms + g_rt_off_sec, n = mn + g_rt_off_nsec;
	if (n >= 1000000000LL) { s++; n -= 1000000000LL; }
	if (n < 0)             { s--; n += 1000000000LL; }
	*sec = s; *nsec = n;
}

/* read the hardware clock (CMOS / PL031) once at boot → real wall-clock epoch */
void rtc_init(void)
{
	extern int64_t arch_rtc_read(void);
	int64_t e = arch_rtc_read();
	if (e > 1000000000LL) {           /* sane epoch (after 2001) */
		int64_t ms, mn; mono_time(&ms, &mn);
		g_rt_off_sec  = e - ms;
		g_rt_off_nsec = -mn;
	}
}

/* filesystem timestamps: real wall-clock once the RTC set it, else uptime. */
void ranix_walltime(int64_t *sec, int64_t *nsec) { real_time(sec, nsec); }

#define CLOCK_PROCESS_CPUTIME_ID 2
#define CLOCK_THREAD_CPUTIME_ID  3
#define CLOCK_MONOTONIC_RAW      4
#define CLOCK_REALTIME_COARSE    5
#define CLOCK_MONOTONIC_COARSE   6
#define CLOCK_TAI                11

/* CPU time consumed by the current task: process_t.ticks counts timer ticks
 * the task was running on-CPU (sched.c). */
static void cpu_time(int64_t *sec, int64_t *nsec)
{
	process_t *p = task_current();
	uint64_t t = p ? p->ticks : 0;
	*sec  = (int64_t)(t / TIMER_HZ);
	*nsec = (int64_t)((t % TIMER_HZ) * (1000000000ULL / TIMER_HZ));
}

static long sys_clock_gettime(int clkid, linux_timespec_t *tp)
{
	if (!tp) return -14;
	linux_timespec_t k;
	switch (clkid) {
	case CLOCK_REALTIME:
	case CLOCK_REALTIME_COARSE:
	case CLOCK_TAI:                /* no leap-second table → TAI == UTC for now */
		real_time(&k.tv_sec, &k.tv_nsec);
		break;
	case CLOCK_PROCESS_CPUTIME_ID:
	case CLOCK_THREAD_CPUTIME_ID:
		cpu_time(&k.tv_sec, &k.tv_nsec);
		break;
	case CLOCK_MONOTONIC:
	case CLOCK_BOOTTIME: {
		mono_time(&k.tv_sec, &k.tv_nsec);
		time_namespace_t *ns = current_time_ns();
		long osec  = (clkid == CLOCK_BOOTTIME) ? ns->boot_sec  : ns->mono_sec;
		long onsec = (clkid == CLOCK_BOOTTIME) ? ns->boot_nsec : ns->mono_nsec;
		k.tv_sec  += osec; k.tv_nsec += onsec;
		if (k.tv_nsec >= 1000000000L) { k.tv_sec++; k.tv_nsec -= 1000000000L; }
		break;
	}
	case CLOCK_MONOTONIC_RAW:      /* raw: no NTP adjust, no namespace offset */
	case CLOCK_MONOTONIC_COARSE:
		mono_time(&k.tv_sec, &k.tv_nsec);
		break;
	default:
		return -22; /* EINVAL: unknown clock */
	}
	return copy_to_user(tp, &k, sizeof(k));
}

static long sys_clock_getres(int clkid, linux_timespec_t *res)
{
	switch (clkid) {
	case CLOCK_REALTIME: case CLOCK_MONOTONIC: case CLOCK_BOOTTIME:
	case CLOCK_MONOTONIC_RAW: case CLOCK_TAI:
	case CLOCK_PROCESS_CPUTIME_ID: case CLOCK_THREAD_CPUTIME_ID:
	case CLOCK_REALTIME_COARSE: case CLOCK_MONOTONIC_COARSE:
		break;
	default: return -22;
	}
	if (res) {
		linux_timespec_t r;
		/* coarse clocks tick at the timer; the rest are ~nanosecond (HPET/cntvct) */
		if (clkid == CLOCK_REALTIME_COARSE || clkid == CLOCK_MONOTONIC_COARSE) {
			r.tv_sec = 0; r.tv_nsec = 1000000000L / TIMER_HZ;
		} else {
			r.tv_sec = 0; r.tv_nsec = 1;
		}
		return copy_to_user(res, &r, sizeof(r));
	}
	return 0;
}


static long sys_gettimeofday(linux_timeval_t *tv, void *tz)
{
	(void)tz;
	if (!tv) return 0;
	linux_timeval_t k;
	int64_t s, ns;
	real_time(&s, &ns);
	k.tv_sec  = s;
	k.tv_usec = ns / 1000;
	return copy_to_user(tv, &k, sizeof(k));
}

static long sys_clock_settime(int clkid, const linux_timespec_t *tp)
{
	if (clkid != CLOCK_REALTIME || !tp) return -22;
	linux_timespec_t k;
	if (copy_from_user(&k, tp, sizeof(k)) != 0) return -14;
	int64_t ms, mn; mono_time(&ms, &mn);
	g_rt_off_sec  = (int64_t)k.tv_sec  - ms;
	g_rt_off_nsec = (int64_t)k.tv_nsec - mn;
	return 0;
}

/* persist the current wall-clock to the hardware RTC (hwclock --systohc). */
static void rtc_sync_to_hw(void)
{
	extern void arch_rtc_write(int64_t epoch_sec);
	int64_t s, n; real_time(&s, &n);
	arch_rtc_write(s);
}

static long sys_settimeofday(const linux_timeval_t *tv, const void *tz)
{
	(void)tz;
	if (!tv) return -14;
	linux_timeval_t k;
	if (copy_from_user(&k, tv, sizeof(k)) != 0) return -14;
	int64_t ms, mn; mono_time(&ms, &mn);
	g_rt_off_sec  = (int64_t)k.tv_sec - ms;
	g_rt_off_nsec = (int64_t)k.tv_usec * 1000 - mn;
	rtc_sync_to_hw();   /* persist to hardware clock — survives reboot */
	return 0;
}

/* struct timex — kernel/musl layout (64-bit). adjtimex is the NTP discipline
 * interface: ntpd uses it to step (ADJ_SETOFFSET) and steer (ADJ_FREQUENCY)
 * the clock. */
typedef struct {
	int32_t modes; int32_t _p0;
	int64_t offset, freq, maxerror, esterror;
	int32_t status; int32_t _p1;
	int64_t constant, precision, tolerance;
	int64_t time_sec, time_usec;       /* struct timeval */
	int64_t tick, ppsfreq, jitter;
	int32_t shift; int32_t _p2;
	int64_t stabil, jitcnt, calcnt, errcnt, stbcnt;
	int32_t tai; int32_t _p3[11];
} k_timex_t;

static int64_t g_tx_freq = 0;   /* steered frequency (scaled ppm, freq/65536 = ppm) */

static long sys_adjtimex(k_timex_t *utx)
{
	if (!utx) return -14;
	k_timex_t tx;
	if (copy_from_user(&tx, utx, sizeof(tx)) != 0) return -14;

	if (tx.modes & 0x0100) {           /* ADJ_SETOFFSET: step the clock by .time */
		int64_t add_s = tx.time_sec;
		int64_t add_n = (tx.modes & 0x2000) ? tx.time_usec   /* ADJ_NANO */
		                                     : tx.time_usec * 1000;
		int64_t ms, mn, cs, cn; mono_time(&ms, &mn); real_time(&cs, &cn);
		int64_t ns = cs + add_s, nn = cn + add_n;
		while (nn >= 1000000000LL) { nn -= 1000000000LL; ns++; }
		while (nn < 0)             { nn += 1000000000LL; ns--; }
		g_rt_off_sec = ns - ms; g_rt_off_nsec = nn - mn;
		rtc_sync_to_hw();
	}
	if (tx.modes & 0x0002) g_tx_freq = tx.freq;   /* ADJ_FREQUENCY: store steer */

	/* report current discipline state */
	tx.offset = 0; tx.freq = g_tx_freq; tx.maxerror = 0; tx.esterror = 0;
	tx.status = 0; tx.tick = 10000; tx.tai = 0;
	copy_to_user(utx, &tx, sizeof(tx));
	return 0;   /* TIME_OK */
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
    process_t *p = task_current();
    uint64_t *value, *interval;
    switch (which) {
    case 0: value = &p->itimer_value; interval = &p->itimer_interval; break; /* REAL    → SIGALRM   */
    case 1: value = &p->vtimer_value; interval = &p->vtimer_interval; break; /* VIRTUAL → SIGVTALRM */
    case 2: value = &p->ptimer_value; interval = &p->ptimer_interval; break; /* PROF    → SIGPROF   */
    default: return -22; /* EINVAL */
    }
    uint64_t hz = TIMER_HZ;
    if (old_v) {
        itimerval_t oval;
        oval.it_value.tv_sec    = (int64_t)(*value / hz);
        oval.it_value.tv_usec   = (int64_t)((*value % hz) * (1000000 / hz));
        oval.it_interval.tv_sec = (int64_t)(*interval / hz);
        oval.it_interval.tv_usec= (int64_t)((*interval % hz) * (1000000 / hz));
        copy_to_user(old_v, &oval, sizeof(oval));
    }
    if (new_v) {
        itimerval_t nval;
        if (copy_from_user(&nval, new_v, sizeof(nval)) != 0) return -14;
        *value    = (uint64_t)nval.it_value.tv_sec * hz +
                    (uint64_t)nval.it_value.tv_usec * hz / 1000000;
        *interval = (uint64_t)nval.it_interval.tv_sec * hz +
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
		process_t *p = task_current();
		int64_t tms[4];   /* tms_utime, tms_stime, tms_cutime, tms_cstime — in ticks (CLK_TCK==TIMER_HZ) */
		tms[0] = (int64_t)(p ? p->utime_ticks  : 0);
		tms[1] = (int64_t)(p ? p->stime_ticks  : 0);
		tms[2] = (int64_t)(p ? p->cutime_ticks : 0);
		tms[3] = (int64_t)(p ? p->cstime_ticks : 0);
		if (copy_to_user(buf, tms, sizeof(tms)) != 0) return -14;
	}
	return (long)kernel_ticks;
}

static long sys_sysinfo(void *info)
{
	if (!info) return 0;
	/* struct sysinfo layout (kernel ABI) */
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

/* ── execve ──────────────────────────────────────────────────────────────────
 * Replace the running process image: read and validate the ELF, build a fresh
 * address space (stack + guard page + loaded segments), apply suid/sgid and the
 * capability transition, lay out the SysV initial stack (argv/envp/auxv), then
 * switch to the new space — freeing the old one only after the switch succeeds.
 * Relies on syscall.c helpers (vma_add, vma_free_all, rng_next) and the
 * USER_STACK_/USER_MMAP_ macros, all in scope via the textual include.
 * ───────────────────────────────────────────────────────────────────────── */
static long sys_execve(const char *path, char * const argv[], char * const envp[]);

/* ── pidfd (glibc 2.36+) ─────────────────────────────────────────────────────
 * A file descriptor referring to a process: the pid lives in the inode and
 * pidfd_send_signal translates into the normal kill path. (poll-on-exit is not
 * wired yet; the signal-sending use that glibc relies on works.) */
/* the pid is stored inline in priv (not heap), so clear it before teardown
 * frees the inode — otherwise fd_table_destroy would kfree a bogus pointer */
static int pidfd_close_vfs(vfs_file_t *f) { if (f->inode) f->inode->priv = NULL; return 0; }
static vfs_fops_t pidfd_fops = { .close = pidfd_close_vfs };

static long sys_pidfd_open(pid_t pid, unsigned int flags)
{
	(void)flags;
	if (pid <= 0 || !proc_find(pid)) return -3;   /* ESRCH */
	vfs_inode_t *in = kmalloc(sizeof(*in));
	if (!in) return -12;
	memset(in, 0, sizeof(*in));
	in->fops     = &pidfd_fops;
	in->priv     = (void *)(uint64_t)pid;
	in->refcount = 1;
	vfs_file_t *f = kmalloc(sizeof(*f));
	if (!f) { kfree(in); return -12; }
	memset(f, 0, sizeof(*f));
	f->inode    = in;
	f->refcount = 0;   /* fd_alloc takes the +1 */
	int fd = fd_alloc(task_current()->fds, f);
	if (fd < 0) { kfree(f); kfree(in); return -12; }
	return fd;
}

static long sys_pidfd_send_signal(int pidfd, int sig, void *info, unsigned int flags)
{
	(void)info; (void)flags;
	vfs_file_t *f = fd_get(task_current()->fds, pidfd);
	if (!f || !f->inode || f->inode->fops != &pidfd_fops) return -9; /* EBADF */
	return sys_kill((pid_t)(uint64_t)f->inode->priv, sig);
}

/* ── real-time scheduling policy (SCHED_FIFO/RR) ─────────────────────────────
 * Policy + priority stored per process; cpu_rq_add runs real-time tasks ahead
 * of SCHED_NORMAL ones. */
struct ranix_sched_param { int sched_priority; };

static long sys_sched_setscheduler(pid_t pid, int policy, const void *uparam)
{
	if (policy != SCHED_NORMAL && policy != SCHED_FIFO && policy != SCHED_RR) return -22;
	process_t *p = pid ? proc_find(pid) : task_current();
	if (!p) return -3;
	int prio = 0;
	if (uparam) {
		struct ranix_sched_param sp;
		if (copy_from_user(&sp, uparam, sizeof(sp)) != 0) return -14;
		prio = sp.sched_priority;
	}
	if (policy == SCHED_NORMAL) { if (prio != 0) return -22; }
	else if (prio < 1 || prio > 99) return -22;
	p->sched_policy = policy;
	p->rt_priority  = prio;
	return 0;
}

static long sys_sched_setparam(pid_t pid, const void *uparam)
{
	process_t *p = pid ? proc_find(pid) : task_current();
	if (!p) return -3;
	if (!uparam) return -14;
	struct ranix_sched_param sp;
	if (copy_from_user(&sp, uparam, sizeof(sp)) != 0) return -14;
	if (p->sched_policy == SCHED_NORMAL) { if (sp.sched_priority != 0) return -22; }
	else if (sp.sched_priority < 1 || sp.sched_priority > 99) return -22;
	p->rt_priority = sp.sched_priority;
	return 0;
}

static long sys_sched_getscheduler(pid_t pid)
{
	process_t *p = pid ? proc_find(pid) : task_current();
	return p ? p->sched_policy : -3;
}

static long sys_sched_getparam(pid_t pid, void *uparam)
{
	process_t *p = pid ? proc_find(pid) : task_current();
	if (!p) return -3;
	struct ranix_sched_param sp = { p->rt_priority };
	if (copy_to_user(uparam, &sp, sizeof(sp)) != 0) return -14;
	return 0;
}

static long sys_sched_get_priority_max(int policy)
{
	if (policy == SCHED_FIFO || policy == SCHED_RR) return 99;
	if (policy == SCHED_NORMAL) return 0;
	return -22;
}

static long sys_sched_get_priority_min(int policy)
{
	if (policy == SCHED_FIFO || policy == SCHED_RR) return 1;
	if (policy == SCHED_NORMAL) return 0;
	return -22;
}

/* execveat(dirfd, path, argv, envp, flags): resolve the dirfd-relative (or
 * AT_EMPTY_PATH) path to an absolute one, then run the normal execve. */
static long sys_execveat(int dirfd, const char *path, char * const argv[],
                         char * const envp[], int flags)
{
	char kp[256];
	if ((flags & AT_EMPTY_PATH) && (!path || !path[0])) {
		vfs_file_t *f = fd_get(task_current()->fds, dirfd);
		if (!f || !f->path[0]) return -9;
		return sys_execve(f->path, argv, envp);
	}
	if (!path) return -14;
	int r = resolve_path(kp, dirfd, path);
	if (r != 0) return r;
	return sys_execve(kp, argv, envp);
}

static long sys_execve(const char *path, char * const argv[], char * const envp[])
{
	if (!path) return -14;

	process_t *p = task_current();

	/* copy path to kernel buffer */
	char kpath[PROC_NAME_MAX];
	int i = 0;
	while (i < PROC_NAME_MAX - 1 && path[i]) {
		kpath[i] = path[i];
		i++;
	}
	kpath[i] = '\0';

	/* copy argv/envp strings to kernel before VM switch */
#define EXEC_MAXARGS 64
#define EXEC_MAXSTRTOTAL 16384
	int argc = 0, envc = 0;
	char *kstrs = kmalloc(EXEC_MAXSTRTOTAL);
	char *kargv[EXEC_MAXARGS + 1];
	char *kenvp[EXEC_MAXARGS + 1];
	int koff = 0;
	if (kstrs) {
		if (argv) {
			for (; argc < EXEC_MAXARGS && argv[argc]; argc++) {
				const char *s = argv[argc];
				int n = 0;
				while (s[n] && koff + n < EXEC_MAXSTRTOTAL - 1) n++;
				memcpy(kstrs + koff, s, n);
				kstrs[koff + n] = '\0';
				kargv[argc] = kstrs + koff;
				koff += n + 1;
			}
		}
		if (envp) {
			for (; envc < EXEC_MAXARGS && envp[envc]; envc++) {
				const char *s = envp[envc];
				int n = 0;
				while (s[n] && koff + n < EXEC_MAXSTRTOTAL - 1) n++;
				memcpy(kstrs + koff, s, n);
				kstrs[koff + n] = '\0';
				kenvp[envc] = kstrs + koff;
				koff += n + 1;
			}
		}
	}

	/* read file from VFS */
	vfs_stat_t st = {0};
	if (vfs_stat(kpath, &st) != 0) { kfree(kstrs); return -2; }
	if (st.size == 0)              { kfree(kstrs); return -2; }

	/* resolve credentials for the new image */
	uid_t new_euid = p->euid;
	gid_t new_egid = p->egid;
	if (st.mode & S_ISUID) new_euid = st.uid;
	if (st.mode & S_ISGID) new_egid = st.gid;

	uint8_t *buf = kmalloc(st.size);
	if (!buf) { kfree(kstrs); return -12; }

	int fd = vfs_open(kpath, O_RDONLY, 0);
	if (fd < 0) { kfree(buf); kfree(kstrs); return -2; }
	vfs_read(fd, buf, st.size);
	vfs_close(fd);

	/* build new address space */
	vm_space_t *new_vm = vm_create();
	if (!new_vm) { kfree(buf); kfree(kstrs); return -12; }

	elf_info_t ei = {0};
	if (elf_load(new_vm, buf, (size_t)st.size, 0, &ei) != 0) {
		kfree(buf);
		vm_destroy(new_vm);
		kfree(kstrs);
		return -8;
	}
	kfree(buf);

	/* entry point and aux values; overridden below for dynamic executables */
	uint64_t jump_entry = ei.entry;   /* where the kernel transfers control */
	uint64_t at_base    = 0;          /* AT_BASE: dynamic linker load base */
	uint64_t interp_lo = 0, interp_hi = 0;
	/* ASLR mmap base; the dynamic linker (if any) loads at its bottom */
	uint64_t mmap_base  = USER_MMAP_BASE_ + ((rng_next() & 0xffffULL) << 12);

	/* Dynamically linked: load the interpreter (ld.so) too and enter it
	 * instead of the executable. It performs relocations in userspace, then
	 * jumps to the executable's entry (passed via AT_ENTRY). */
	if (ei.interp[0]) {
		vfs_stat_t ist = {0};
		if (vfs_stat(ei.interp, &ist) != 0 || ist.size == 0) {
			vm_destroy(new_vm); kfree(kstrs); return -8;
		}
		uint8_t *ibuf = kmalloc(ist.size);
		if (!ibuf) { vm_destroy(new_vm); kfree(kstrs); return -12; }
		int ifd = vfs_open(ei.interp, O_RDONLY, 0);
		if (ifd < 0) { kfree(ibuf); vm_destroy(new_vm); kfree(kstrs); return -8; }
		vfs_read(ifd, ibuf, ist.size);
		vfs_close(ifd);

		elf_info_t ii = {0};
		if (elf_load(new_vm, ibuf, (size_t)ist.size, mmap_base, &ii) != 0) {
			kfree(ibuf); vm_destroy(new_vm); kfree(kstrs); return -8;
		}
		kfree(ibuf);

		jump_entry = ii.entry;
		at_base    = mmap_base;
		interp_lo  = ii.load_bias;
		interp_hi  = ii.load_end;
		/* keep subsequent mmap allocations above the interpreter image */
		mmap_base  = (ii.load_end + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
	}

	/* map user stack */
	for (int j = 0; j < USER_STACK_PAGES_; j++) {
		void *pg = buddy_alloc(0);
		if (!pg) { vm_destroy(new_vm); kfree(kstrs); return -12; }
		memset((void *)PHYS_TO_VIRT(pg), 0, PAGE_SIZE);
		uint64_t sva = USER_STACK_TOP_ - (uint64_t)(USER_STACK_PAGES_ - j) * PAGE_SIZE;
		vm_map(new_vm, sva, (uint64_t)pg, VM_USER | VM_READ | VM_WRITE);
	}
	{
		void *guard = buddy_alloc(0);
		if (guard) {
			memset((void *)PHYS_TO_VIRT(guard), 0, PAGE_SIZE);
			uint64_t guard_va = USER_STACK_TOP_ - (uint64_t)(USER_STACK_PAGES_ + 1) * PAGE_SIZE;
			vm_map(new_vm, guard_va, (uint64_t)guard, VM_USER | VM_READ);
		}
	}

	/* update process name */
	const char *last_slash = kpath;
	for (int j = 0; kpath[j]; j++) if (kpath[j] == '/') last_slash = kpath + j + 1;

	int nlen = 0;
	while (last_slash[nlen] && nlen < PROC_NAME_MAX - 1) {
		p->name[nlen] = last_slash[nlen];
		nlen++;
	}
	p->name[nlen] = '\0';

	/* remember the full path for /proc/<pid>/exe */
	int el = 0;
	while (kpath[el] && el < PROC_NAME_MAX - 1) { p->exe_path[el] = kpath[el]; el++; }
	p->exe_path[el] = '\0';

	/* close O_CLOEXEC file descriptors before exec */
	if (p->fds) {
		for (int i = 0; i < p->fds->cap; i++) {
			if (!fd_is_cloexec(p->fds, i)) continue;
			vfs_file_t *xf = fd_get(p->fds, i);
			if (!xf) { fd_set_cloexec(p->fds, i, 0); continue; }
			fd_free(p->fds, i);
			if (--xf->refcount == 0) {
				if (xf->inode->fops && xf->inode->fops->close)
					xf->inode->fops->close(xf);
				kfree(xf);
			}
		}
	}

	/* reset signal state: SIG_IGN stays, user handlers reset to SIG_DFL. If the
	 * disposition table is still shared with sibling threads, detach to a private
	 * copy first so the reset doesn't reach them. */
	if (p->sighand->refcount > 1) {
		sighand_t *priv = sighand_alloc(p->sighand);
		if (priv) { sighand_put(p->sighand); p->sighand = priv; }
	}
	for (int i = 0; i < NSIG; i++) {
		if (p->sighand->actions[i].sa_handler != (void (*)(int))1 /* SIG_IGN */)
			p->sighand->actions[i] = (sigaction_t){0};
	}
	p->sig_mask    = 0;
	p->sig_pending = 0;

	/* apply credentials: suid/sgid set to new euid/egid after exec */
	p->euid = new_euid; p->suid = new_euid;
	p->egid = new_egid; p->sgid = new_egid;

	/* capability transition on exec (no file caps):
	 * P'(permitted)   = P(inheritable)
	 * P'(effective)   = P'(permitted)   (if new euid == 0, keep all)
	 * P'(inheritable) = P(inheritable)  (unchanged)
	 * Root gets full caps, others keep only inherited subset */
	if (new_euid == 0) {
		p->cap_permitted   = CAP_FULL;
		p->cap_effective   = CAP_FULL;
	} else {
		p->cap_permitted   = p->cap_inheritable;
		p->cap_effective   = p->cap_inheritable;
	}

	/* switch to new address space, freeing the old one */
	vma_free_all(p);
	vm_space_t *old_vm = p->vm;
	p->vm        = new_vm;
	p->brk       = ei.load_end;
	p->mmap_next = mmap_base;
	p->dumpable  = 1;
	arch_vm_switch((uint64_t)new_vm->root, new_vm->asid);
	if (old_vm) vm_destroy(old_vm);

	/* record initial VMAs */
	vma_add(p, USER_STACK_TOP_ - USER_STACK_PAGES_ * PAGE_SIZE,
	        USER_STACK_PAGES_ * PAGE_SIZE, VMA_USER | VMA_READ | VMA_WRITE);
	vma_add(p, ei.load_bias, ei.load_end - ei.load_bias,
	        VMA_USER | VMA_READ | VMA_WRITE | VMA_EXEC);
	if (interp_hi > interp_lo)
		vma_add(p, interp_lo, interp_hi - interp_lo,
		        VMA_USER | VMA_READ | VMA_WRITE | VMA_EXEC);

	/* build SysV ABI initial stack */
	uint64_t uargv[EXEC_MAXARGS + 1];
	uint64_t uenvp[EXEC_MAXARGS + 1];
	memset(uargv, 0, sizeof(uargv));
	memset(uenvp, 0, sizeof(uenvp));

	/* ASLR: randomize stack top within last stack page (up to 4080 bytes) */
	uint64_t sp = USER_STACK_TOP_ - ((rng_next() & 0xffULL) << 4);
	if (kstrs) {
		for (int j = envc - 1; j >= 0; j--) {
			int n = 0; while (kenvp[j][n]) n++;
			sp -= (uint64_t)(n + 1);
			memcpy((void *)sp, kenvp[j], (size_t)(n + 1));
			uenvp[j] = sp;
		}
		for (int j = argc - 1; j >= 0; j--) {
			int n = 0; while (kargv[j][n]) n++;
			sp -= (uint64_t)(n + 1);
			memcpy((void *)sp, kargv[j], (size_t)(n + 1));
			uargv[j] = sp;
		}
		kfree(kstrs);
	}
	sp &= ~15ULL;

	/* 16 random bytes for AT_RANDOM (used by libc for stack canary, etc.) */
	sp -= 16;
	{
		uint64_t *rp = (uint64_t *)sp;
		rp[0] = rng_next();
		rp[1] = rng_next();
	}
	uint64_t at_random_addr = sp;

	/* execfn string */
	{
		int plen = 0; while (kpath[plen]) plen++;
		sp -= (uint64_t)(plen + 1);
		memcpy((void *)sp, kpath, (size_t)(plen + 1));
	}
	uint64_t at_execfn_addr = sp;

	sp &= ~15ULL;

	/* padding before auxv keeps the final sp 16-byte aligned;
	   must NOT be placed between envp[] and auxv — musl would read it as AT_NULL */
	if ((argc + envc + 3) % 2 != 0) {
		sp -= 8;
		*(uint64_t *)sp = 0;
	}

/* push one auxv key/value pair (16 bytes each, stack grows down) */
#define PUSH_AUX(k, v) do { \
	sp -= 16; \
	((uint64_t *)sp)[0] = (uint64_t)(k); \
	((uint64_t *)sp)[1] = (uint64_t)(v); \
} while (0)

	PUSH_AUX(0,  0);
	PUSH_AUX(31, at_execfn_addr);
	PUSH_AUX(25, at_random_addr);
	PUSH_AUX(23, 0);
	PUSH_AUX(26, 0);   /* AT_HWCAP2 — no extra feature bits advertised */
	PUSH_AUX(17, TIMER_HZ);
#if defined(__aarch64__)
	PUSH_AUX(16, 3);   /* AT_HWCAP: HWCAP_FP | HWCAP_ASIMD */
#else
	PUSH_AUX(16, 0);   /* AT_HWCAP: x86-64 glibc reads CPUID directly */
#endif
	PUSH_AUX(14, 0);
	PUSH_AUX(13, 0);
	PUSH_AUX(12, 0);
	PUSH_AUX(11, 0);
	PUSH_AUX(9,  ei.entry);
	PUSH_AUX(7,  at_base);
	PUSH_AUX(6,  PAGE_SIZE);
	PUSH_AUX(5,  ei.phnum);
	PUSH_AUX(4,  ei.phentsize);
	PUSH_AUX(3,  ei.phdr_va);
#undef PUSH_AUX

	/* envp[] */
	sp -= 8; *(uint64_t *)sp = 0;
	for (int j = envc - 1; j >= 0; j--) { sp -= 8; *(uint64_t *)sp = uenvp[j]; }
	/* argv[] */
	sp -= 8; *(uint64_t *)sp = 0;
	for (int j = argc - 1; j >= 0; j--) { sp -= 8; *(uint64_t *)sp = uargv[j]; }
	/* argc */
	sp -= 8; *(uint64_t *)sp = (uint64_t)argc;

	/* reinitialise context frame */
	arch_context_init_user(p->ctx, jump_entry, sp);

	/* reset TLS */
	p->tls_base = 0;
#if defined(__aarch64__)
	__asm__ volatile("msr tpidr_el0, xzr");
#elif defined(__x86_64__)
	__asm__ volatile("wrmsr" :: "c"(0xC0000100U), "a"(0U), "d"(0U));
#endif

	/* wake vfork parent — child now has its own address space */
	proc_vfork_exec_notify();

	return 0;
}

/* ── process lifecycle ───────────────────────────────────────────────────────
 * exit, fork/vfork (both give the child its own address space — see vfork note),
 * waitpid (reap a child or block until one is reapable), getpid, pipe2, and the
 * x86 arch_prctl (set FS/GS base for TLS). The heavy lifting lives in sched.c
 * (proc_fork, proc_reap, the wait helpers); these are the syscall entry points.
 * ───────────────────────────────────────────────────────────────────────── */

static long sys_exit(int code)
{
	process_t *p = task_current();
	p->exit_code  = code;
	vma_free_all(p);
	task_exit();
	return 0;
}

static long sys_getpid(void)
{
	process_t *p = task_current();
	pid_t glead = p->tgid ? p->tgid : p->pid;
	process_t *leader = (glead == (pid_t)p->pid) ? p : proc_find(glead);
	int nr = task_pid_nr_ns(leader ? leader : p, current_pid_ns());
	return nr ? (long)nr : (long)glead;   /* nr==global in the init ns */
}

static long sys_fork(void)
{
	return (long)proc_fork(0, 0);
}

/* process_vm_readv/writev: copy between this task and another process's address
 * space, walking the local/remote iovecs in lockstep. Same vm-switch + bounce
 * mechanism as /proc/<pid>/mem (a remote page is read under the target's root,
 * then copied out under ours). */
static long sys_process_vm(pid_t pid, const iovec_t *uliov, unsigned long lcnt,
                           const iovec_t *uriov, unsigned long rcnt,
                           unsigned long flags, int write)
{
	(void)flags;
	if (lcnt > 16 || rcnt > 16) return -22;        /* EINVAL (we cap at 16 iovecs) */
	process_t *t = proc_find(pid);
	if (!t || !t->vm) return -3;                   /* ESRCH */
	process_t *cur = task_current();
	if (cur->euid != 0 && cur->euid != t->euid) return -1;   /* EPERM */

	iovec_t liov[16], riov[16];
	if (copy_from_user(liov, uliov, lcnt * sizeof(iovec_t)) != 0) return -14;
	if (copy_from_user(riov, uriov, rcnt * sizeof(iovec_t)) != 0) return -14;

	char tmp[256];
	long total = 0;
	unsigned long li = 0, ri = 0;
	size_t lo = 0, ro = 0;
	while (li < lcnt && ri < rcnt) {
		if (lo >= liov[li].iov_len) { li++; lo = 0; continue; }
		if (ro >= riov[ri].iov_len) { ri++; ro = 0; continue; }
		size_t n = liov[li].iov_len - lo;
		size_t rrem = riov[ri].iov_len - ro;
		if (rrem < n) n = rrem;
		if (n > sizeof(tmp)) n = sizeof(tmp);
		uint64_t lva = (uint64_t)liov[li].iov_base + lo;
		uint64_t rva = (uint64_t)riov[ri].iov_base + ro;
		if (write) {
			if (copy_from_user(tmp, (void *)lva, n) != 0) break;
			arch_vm_switch((uint64_t)t->vm->root, t->vm->asid);
			memcpy((void *)rva, tmp, n);
			arch_vm_switch((uint64_t)cur->vm->root, cur->vm->asid);
		} else {
			arch_vm_switch((uint64_t)t->vm->root, t->vm->asid);
			memcpy(tmp, (void *)rva, n);
			arch_vm_switch((uint64_t)cur->vm->root, cur->vm->asid);
			if (copy_to_user((void *)lva, tmp, n) != 0) break;
		}
		lo += n; ro += n; total += (long)n;
	}
	return total;
}

static long sys_vfork(uint64_t newsp)
{
	/* vfork-as-fork: give the child its own address space instead of
	 * sharing the parent's stack. Sharing is unsafe under SMP — the
	 * parent and child can run concurrently and corrupt the shared
	 * user stack. fork semantics are always a valid vfork. The child still
	 * runs on its own stack when the caller passes one (posix_spawn). */
	return (long)proc_fork(0, newsp);
}

static long sys_pipe2(int *pipefd, int flags)
{
	int kfds[2];
	if (vfs_pipe(kfds) != 0) return -1;
	process_t *p = task_current();
	if (flags & O_NONBLOCK) {
		for (int i = 0; i < 2; i++) {
			vfs_file_t *f = fd_get(p->fds, kfds[i]);
			if (f) f->flags |= O_NONBLOCK;
		}
	}
	if (flags & O_CLOEXEC) {
		fd_set_cloexec(p->fds, kfds[0], 1);
		fd_set_cloexec(p->fds, kfds[1], 1);
	}
	if (copy_to_user(pipefd, kfds, sizeof(int) * 2) != 0) return -14;
	return 0;
}

#define WNOHANG 1

extern void proc_find_zombie_or_sleep(pid_t ppid, process_t *parent, int will_sleep,
                                      process_t **out_child, int *out_any_alive);
extern int  proc_check_or_sleep(process_t *child, process_t *parent, int will_sleep);

/* wait4 rusage: the reaped child's CPU time, including descendants it waited
 * for (Linux folds those into the child's own ru_*). Must run before proc_reap
 * frees the struct. */
static void fill_wait_rusage(void *urusage, process_t *child)
{
	if (!urusage || (uint64_t)urusage <= 0x1000) return;
	int64_t r[18];
	memset(r, 0, sizeof(r));
	uint64_t hz = TIMER_HZ;
	uint64_t ut = child->utime_ticks + child->cutime_ticks;
	uint64_t st = child->stime_ticks + child->cstime_ticks;
	r[0] = (int64_t)(ut / hz); r[1] = (int64_t)((ut % hz) * (1000000 / hz));
	r[2] = (int64_t)(st / hz); r[3] = (int64_t)((st % hz) * (1000000 / hz));
	copy_to_user(urusage, r, sizeof(r));
}

static long sys_waitpid(pid_t pid, int *status, int flags, void *rusage)
{
	process_t *p = task_current();
	int wnohang  = (flags & WNOHANG) != 0;

	if (pid == -1) {
		while (1) {
			process_t *child = NULL;
			int any_alive = 0;
			proc_find_zombie_or_sleep(p->pid, p, !wnohang, &child, &any_alive);
			if (child) {
				int st = child->killed_by_sig
				       ? (child->exit_code & 0x7F)
				       : (child->exit_code & 0xFF) << 8;
				int v = task_pid_nr_ns(child, current_pid_ns());
				pid_t result = v ? (pid_t)v : (pid_t)child->pid;
				fill_wait_rusage(rusage, child);
				proc_reap(child);
				if (status && (uint64_t)status > 0x1000)
					*status = st;
				return (long)result;
			}
			if (!any_alive) return -10; /* ECHILD */
			if (wnohang)    return 0;
			arch_yield();
		}
	}

	process_t *child = pidns_find(current_pid_ns(), (int)pid);
	if (!child || child->ppid != p->pid) return -10;

	while (1) {
		int r = proc_check_or_sleep(child, p, !wnohang);
		if (r == 1) break;  /* zombie */
		if (r == 2) {       /* ptrace stop — ptrace_stop_reported already set */
			if (status && (uint64_t)status > 0x1000)
				*status = (5 << 8) | 0x7f; /* WIFSTOPPED | SIGTRAP */
			return (long)pid;
		}
		if (wnohang) return 0;
		arch_yield();
	}

	child->reaped = 1;
	int st = child->killed_by_sig
	       ? (child->exit_code & 0x7F)
	       : (child->exit_code & 0xFF) << 8;
	fill_wait_rusage(rusage, child);
	proc_reap(child);
	if (status && (uint64_t)status > 0x1000)
		*status = st;
	return (long)pid;
}

#if defined(__x86_64__)
#define ARCH_SET_GS 0x1001
#define ARCH_SET_FS 0x1002

static long sys_arch_prctl(int code, uint64_t addr)
{
	if (code == ARCH_SET_FS) {
		__asm__ volatile("wrmsr" :: "c"(0xC0000100U), "a"((uint32_t)addr), "d"((uint32_t)(addr >> 32)));
		task_current()->tls_base = addr;
		return 0;
	}
	if (code == ARCH_SET_GS) {
		/* When kernel is running, USER GS is stored in KERNEL_GS_BASE MSR. */
		__asm__ volatile("wrmsr" :: "c"(0xC0000102U), "a"((uint32_t)addr), "d"((uint32_t)(addr >> 32)));
		return 0;
	}
	return -22;
}
#endif

