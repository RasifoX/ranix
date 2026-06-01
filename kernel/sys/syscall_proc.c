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
		/* event-driven: woken by futex_do_wake or task_signal. timed waits also
		 * arm wake_deadline so the sched_tick deadline sweep fires the timeout —
		 * no per-tick blanket poll either way. */
		self->sleeping_on_queue = 1;
		if (timed) self->wake_deadline = end;
		spin_unlock_irq(&sched_lock, fs);
		spin_unlock_irq(&futex_lock, f);

		for (;;) {
			arch_yield();
			f = spin_lock_irq(&futex_lock);
			if (q.woken) { self->wake_deadline = 0; spin_unlock_irq(&futex_lock, f); return 0; }
			int sig = (self->sig_pending & ~self->sig_mask) != 0;
			uint32_t c2;
			int vchg = (copy_from_user(&c2, uaddr, sizeof(c2)) == 0 && c2 != val);
			int tmo  = timed && kernel_ticks >= end;
			if (sig || vchg || tmo) {
				self->wake_deadline = 0;
				futex_unlink(&q);
				spin_unlock_irq(&futex_lock, f);
				return sig ? -4 : (tmo ? -110 : -11);
			}
			fs = spin_lock_irq(&sched_lock);
			self->state = PROC_SLEEPING;
			self->sleeping_on_queue = 1;
			if (timed) self->wake_deadline = end;
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
	if (elf_load(new_vm, buf, (size_t)st.size, &ei) != 0) {
		kfree(buf);
		vm_destroy(new_vm);
		kfree(kstrs);
		return -8;
	}
	kfree(buf);

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

	/* reset signal state: SIG_IGN stays, user handlers reset to SIG_DFL */
	for (int i = 0; i < NSIG; i++) {
		if (p->sig_actions[i].sa_handler != (void (*)(int))1 /* SIG_IGN */)
			p->sig_actions[i] = (sigaction_t){0};
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
	/* ASLR: randomize mmap base within a 1 GB window (page-aligned) */
	p->mmap_next = USER_MMAP_BASE_ + ((rng_next() & 0xffffULL) << 12);
	p->dumpable  = 1;
	arch_vm_switch((uint64_t)new_vm->root);
	if (old_vm) vm_destroy(old_vm);

	/* record initial VMAs */
	vma_add(p, USER_STACK_TOP_ - USER_STACK_PAGES_ * PAGE_SIZE,
	        USER_STACK_PAGES_ * PAGE_SIZE, VMA_USER | VMA_READ | VMA_WRITE);
	vma_add(p, ei.load_bias, ei.load_end - ei.load_bias,
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
	PUSH_AUX(17, TIMER_HZ);
#if defined(__aarch64__)
	PUSH_AUX(16, 3);
#else
	PUSH_AUX(16, 0);
#endif
	PUSH_AUX(14, 0);
	PUSH_AUX(13, 0);
	PUSH_AUX(12, 0);
	PUSH_AUX(11, 0);
	PUSH_AUX(9,  ei.entry);
	PUSH_AUX(7,  0);
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
	arch_context_init_user(p->ctx, ei.entry, sp);

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
	return (long)(p->tgid ? p->tgid : p->pid);
}

static long sys_fork(void)
{
	return (long)proc_fork();
}

static long sys_vfork(void)
{
	/* vfork-as-fork: give the child its own address space instead of
	 * sharing the parent's stack. Sharing is unsafe under SMP — the
	 * parent and child can run concurrently and corrupt the shared
	 * user stack. fork semantics are always a valid vfork. */
	return (long)proc_fork();
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

static long sys_waitpid(pid_t pid, int *status, int flags)
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
				pid_t result = child->pid;
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

	process_t *child = proc_find(pid);
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

