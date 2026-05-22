/* ── uname ───────────────────────────────────────────────── */

typedef struct {
	char sysname[65];
	char nodename[65];
	char release[65];
	char version[65];
	char machine[65];
	char domainname[65];
} linux_utsname_t;

static char ranix_hostname[65] = "ranix";

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
#define FUTEX_PRIVATE_FLAG 128

static struct {
	uint32_t  *uaddr;
	process_t *waiter;
} futex_table[FUTEX_SLOTS];
static spinlock_t futex_lock = SPINLOCK_INIT;

static long sys_futex(uint32_t *uaddr, int op, uint32_t val,
                      linux_timespec_t *timeout, uint32_t *uaddr2, uint32_t val3)
{
	(void)uaddr2; (void)val3;
	int cmd = op & ~FUTEX_PRIVATE_FLAG;

	if (cmd == FUTEX_WAIT) {
		uint64_t f = spin_lock_irq(&futex_lock);
		uint32_t cur;
		if (copy_from_user(&cur, uaddr, sizeof(cur)) != 0) {
			spin_unlock_irq(&futex_lock, f);
			return -14;
		}
		if (cur != val) {
			spin_unlock_irq(&futex_lock, f);
			return -11; /* EAGAIN */
		}
		int slot = -1;
		for (int i = 0; i < FUTEX_SLOTS; i++) {
			if (!futex_table[i].waiter) { slot = i; break; }
		}
		if (slot < 0) { spin_unlock_irq(&futex_lock, f); return -11; }
		futex_table[slot].uaddr  = uaddr;
		futex_table[slot].waiter = task_current();
		spin_unlock_irq(&futex_lock, f);

		if (timeout) {
			linux_timespec_t kts;
			if (copy_from_user(&kts, timeout, sizeof(kts)) == 0) {
				uint64_t end = kernel_ticks +
				    (uint64_t)kts.tv_sec * TIMER_HZ +
				    (uint64_t)kts.tv_nsec / (1000000000 / TIMER_HZ);
				while (futex_table[slot].waiter && kernel_ticks < end)
					task_sleep();
			}
		} else {
			while (futex_table[slot].waiter)
				task_sleep();
		}

		f = spin_lock_irq(&futex_lock);
		futex_table[slot].waiter = NULL;
		futex_table[slot].uaddr  = NULL;
		spin_unlock_irq(&futex_lock, f);
		return 0;
	}

	if (cmd == FUTEX_WAKE) {
		uint64_t f = spin_lock_irq(&futex_lock);
		int woken = 0;
		for (int i = 0; i < FUTEX_SLOTS && woken < (int)val; i++) {
			if (futex_table[i].uaddr == uaddr && futex_table[i].waiter) {
				task_wake(futex_table[i].waiter);
				futex_table[i].waiter = NULL;
				futex_table[i].uaddr  = NULL;
				woken++;
			}
		}
		spin_unlock_irq(&futex_lock, f);
		return woken;
	}

	return -38;
}

static long sys_clock_gettime(int clkid, linux_timespec_t *tp)
{
	(void)clkid;
	if (!tp) return -14;
	linux_timespec_t k;
	k.tv_sec  = (int64_t)(kernel_ticks / TIMER_HZ);
	k.tv_nsec = (int64_t)((kernel_ticks % TIMER_HZ) * (1000000000 / TIMER_HZ));
	return copy_to_user(tp, &k, sizeof(k));
}

static long sys_gettimeofday(linux_timeval_t *tv, void *tz)
{
	(void)tz;
	if (!tv) return 0;
	linux_timeval_t k;
	k.tv_sec  = (int64_t)(kernel_ticks / TIMER_HZ);
	k.tv_usec = (int64_t)((kernel_ticks % TIMER_HZ) * (1000000 / TIMER_HZ));
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
		task_sleep();
		if (cp->sig_pending & ~cp->sig_mask) return -4; /* EINTR */
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

