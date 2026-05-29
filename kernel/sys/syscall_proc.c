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

static struct {
	uint32_t  *uaddr;
	process_t *waiter;
} futex_table[FUTEX_SLOTS];
static spinlock_t futex_lock = SPINLOCK_INIT;

static long sys_futex(uint32_t *uaddr, int op, uint32_t val,
                      linux_timespec_t *timeout, uint32_t *uaddr2, uint32_t val3)
{
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

		process_t *self = task_current();
		if (timeout) {
			linux_timespec_t kts;
			if (copy_from_user(&kts, timeout, sizeof(kts)) == 0) {
				uint64_t end = kernel_ticks +
				    (uint64_t)kts.tv_sec * TIMER_HZ +
				    (uint64_t)kts.tv_nsec / (1000000000 / TIMER_HZ);
				while (kernel_ticks < end) {
					uint64_t fq = spin_lock_irq(&futex_lock);
					if (!futex_table[slot].waiter) {
						spin_unlock_irq(&futex_lock, fq);
						break;
					}
					uint64_t fs = spin_lock_irq(&sched_lock);
					self->state = PROC_SLEEPING;
					int sig = (self->sig_pending & ~self->sig_mask) != 0;
					if (sig) self->state = PROC_RUNNING;
					spin_unlock_irq(&sched_lock, fs);
					spin_unlock_irq(&futex_lock, fq);
					if (sig) break;
					arch_yield();
					if (self->sig_pending & ~self->sig_mask) break;
				}
			}
		} else {
			while (1) {
				uint64_t fq = spin_lock_irq(&futex_lock);
				if (!futex_table[slot].waiter) {
					spin_unlock_irq(&futex_lock, fq);
					break;
				}
				uint64_t fs = spin_lock_irq(&sched_lock);
				self->state = PROC_SLEEPING;
				int sig = (self->sig_pending & ~self->sig_mask) != 0;
				if (sig) self->state = PROC_RUNNING;
				spin_unlock_irq(&sched_lock, fs);
				spin_unlock_irq(&futex_lock, fq);
				if (sig) break;
				arch_yield();
				if (self->sig_pending & ~self->sig_mask) break;
			}
		}

		f = spin_lock_irq(&futex_lock);
		int was_woken = (futex_table[slot].waiter == NULL);
		futex_table[slot].waiter = NULL;
		futex_table[slot].uaddr  = NULL;
		spin_unlock_irq(&futex_lock, f);
		return was_woken ? 0 : -4; /* EINTR if not woken by WAKE */
	}

	if (cmd == FUTEX_WAKE) {
		uint64_t f = spin_lock_irq(&futex_lock);
		int woken = 0;
		for (int i = 0; i < FUTEX_SLOTS && woken < (int)val; i++) {
			if (futex_table[i].uaddr == uaddr && futex_table[i].waiter) {
				task_wake_no_resched(futex_table[i].waiter);
				futex_table[i].waiter = NULL;
				futex_table[i].uaddr  = NULL;
				woken++;
			}
		}
		spin_unlock_irq(&futex_lock, f);
		return woken;
	}

	if (cmd == FUTEX_WAKE_OP) {
		if (!uaddr2) return -22;
		/* val3 encoding: op[31:28] cmp[27:24] oparg[23:12] cmparg[11:0] */
		uint32_t op3     = (val3 >> 28) & 0xf;
		uint32_t cmp3    = (val3 >> 24) & 0xf;
		uint32_t oparg   = (val3 >> 12) & 0xfff;
		uint32_t cmparg  = val3 & 0xfff;
		if (op3 & 8) { oparg = 1u << oparg; op3 &= ~8u; } /* FUTEX_OP_ARG_SHIFT */
		uint32_t oldval = 0;
		if (copy_from_user(&oldval, uaddr2, sizeof(oldval)) != 0) return -14;
		uint32_t newval = oldval;
		switch (op3) {
		case 0: newval  = oparg; break;           /* SET */
		case 1: newval += oparg; break;           /* ADD */
		case 2: newval |= oparg; break;           /* OR  */
		case 3: newval &= ~oparg; break;          /* ANDN */
		case 4: newval ^= oparg; break;           /* XOR */
		}
		if (copy_to_user(uaddr2, &newval, sizeof(newval)) != 0) return -14;
		/* wake val waiters on uaddr */
		uint64_t f = spin_lock_irq(&futex_lock);
		int woken = 0;
		for (int i = 0; i < FUTEX_SLOTS && woken < (int)val; i++) {
			if (futex_table[i].uaddr == uaddr && futex_table[i].waiter) {
				task_wake_no_resched(futex_table[i].waiter);
				futex_table[i].waiter = NULL;
				futex_table[i].uaddr  = NULL;
				woken++;
			}
		}
		/* wake val2 waiters on uaddr2 if cmp holds */
		int do_wake2 = 0;
		switch (cmp3) {
		case 0: do_wake2 = (oldval == cmparg); break;
		case 1: do_wake2 = (oldval != cmparg); break;
		case 2: do_wake2 = ((int32_t)oldval <  (int32_t)cmparg); break;
		case 3: do_wake2 = ((int32_t)oldval <= (int32_t)cmparg); break;
		case 4: do_wake2 = ((int32_t)oldval >  (int32_t)cmparg); break;
		case 5: do_wake2 = ((int32_t)oldval >= (int32_t)cmparg); break;
		}
		if (do_wake2) {
			/* val2 is passed as the timeout pointer cast to uintptr_t */
			int val2 = (int)(uintptr_t)timeout;
			for (int i = 0; i < FUTEX_SLOTS && woken < (int)val + val2; i++) {
				if (futex_table[i].uaddr == uaddr2 && futex_table[i].waiter) {
					task_wake_no_resched(futex_table[i].waiter);
					futex_table[i].waiter = NULL;
					futex_table[i].uaddr  = NULL;
					woken++;
				}
			}
		}
		spin_unlock_irq(&futex_lock, f);
		return woken;
	}

	if (cmd == FUTEX_REQUEUE || cmd == (FUTEX_REQUEUE|FUTEX_PRIVATE_FLAG) ||
	    cmd == 4 /* FUTEX_CMP_REQUEUE */ || cmd == (4|FUTEX_PRIVATE_FLAG)) {
		/* REQUEUE: wake up to val waiters on uaddr, requeue rest to uaddr2 */
		if (cmd == 4 || cmd == (4|FUTEX_PRIVATE_FLAG)) {
			uint32_t cur;
			uint64_t fq = spin_lock_irq(&futex_lock);
			if (copy_from_user(&cur, uaddr, sizeof(cur)) != 0) {
				spin_unlock_irq(&futex_lock, fq);
				return -14;
			}
			if (cur != val3) { spin_unlock_irq(&futex_lock, fq); return -11; }
			spin_unlock_irq(&futex_lock, fq);
		}
		uint64_t f = spin_lock_irq(&futex_lock);
		int woken = 0;
		int requeued = 0;
		int max_wake = (int)val;
		int max_req  = (int)(uintptr_t)timeout; /* val2 passed as timeout ptr */
		for (int i = 0; i < FUTEX_SLOTS; i++) {
			if (futex_table[i].uaddr != uaddr || !futex_table[i].waiter) continue;
			if (woken < max_wake) {
				task_wake(futex_table[i].waiter);
				futex_table[i].waiter = NULL;
				futex_table[i].uaddr  = NULL;
				woken++;
			} else if (requeued < max_req && uaddr2) {
				futex_table[i].uaddr = uaddr2;
				requeued++;
			}
		}
		spin_unlock_irq(&futex_lock, f);
		return woken;
	}

	/* FUTEX_WAIT_BITSET (9) — treat like WAIT, ignore bitset */
	if ((cmd & ~FUTEX_PRIVATE_FLAG) == 9) {
		uint64_t f = spin_lock_irq(&futex_lock);
		uint32_t cur;
		if (copy_from_user(&cur, uaddr, sizeof(cur)) != 0) { spin_unlock_irq(&futex_lock, f); return -14; }
		if (cur != val) { spin_unlock_irq(&futex_lock, f); return -11; }
		int slot = -1;
		for (int i = 0; i < FUTEX_SLOTS; i++) { if (!futex_table[i].waiter) { slot = i; break; } }
		if (slot < 0) { spin_unlock_irq(&futex_lock, f); return -11; }
		futex_table[slot].uaddr  = uaddr;
		futex_table[slot].waiter = task_current();
		spin_unlock_irq(&futex_lock, f);
		process_t *self2 = task_current();
		if (timeout) {
			linux_timespec_t kts;
			if (copy_from_user(&kts, timeout, sizeof(kts)) == 0) {
				uint64_t end = kernel_ticks + (uint64_t)kts.tv_sec * TIMER_HZ +
				               (uint64_t)kts.tv_nsec / (1000000000 / TIMER_HZ);
				while (kernel_ticks < end) {
					uint64_t fq = spin_lock_irq(&futex_lock);
					if (!futex_table[slot].waiter) {
						spin_unlock_irq(&futex_lock, fq);
						break;
					}
					uint64_t fs = spin_lock_irq(&sched_lock);
					self2->state = PROC_SLEEPING;
					int sig = (self2->sig_pending & ~self2->sig_mask) != 0;
					if (sig) self2->state = PROC_RUNNING;
					spin_unlock_irq(&sched_lock, fs);
					spin_unlock_irq(&futex_lock, fq);
					if (sig) break;
					arch_yield();
					if (self2->sig_pending & ~self2->sig_mask) break;
				}
			}
		} else {
			while (1) {
				uint64_t fq = spin_lock_irq(&futex_lock);
				if (!futex_table[slot].waiter) {
					spin_unlock_irq(&futex_lock, fq);
					break;
				}
				uint64_t fs = spin_lock_irq(&sched_lock);
				self2->state = PROC_SLEEPING;
				int sig = (self2->sig_pending & ~self2->sig_mask) != 0;
				if (sig) self2->state = PROC_RUNNING;
				spin_unlock_irq(&sched_lock, fs);
				spin_unlock_irq(&futex_lock, fq);
				if (sig) break;
				arch_yield();
				if (self2->sig_pending & ~self2->sig_mask) break;
			}
		}
		f = spin_lock_irq(&futex_lock);
		int woken2 = (futex_table[slot].waiter == NULL);
		futex_table[slot].waiter = NULL;
		futex_table[slot].uaddr  = NULL;
		spin_unlock_irq(&futex_lock, f);
		return woken2 ? 0 : -4;
	}

	return -38;
}

/* Called from sched.c task_exit to wake futex waiters on clear_child_tid */
void futex_wake_addr(uint32_t *uaddr)
{
	if (!uaddr) return;
	uint64_t f = spin_lock_irq(&futex_lock);
	for (int i = 0; i < FUTEX_SLOTS; i++) {
		if (futex_table[i].uaddr == uaddr && futex_table[i].waiter) {
			task_wake(futex_table[i].waiter);
			futex_table[i].waiter = NULL;
			futex_table[i].uaddr  = NULL;
		}
	}
	spin_unlock_irq(&futex_lock, f);
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

