/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
#include <kernel/board.h>
#include <kernel/capabilities.h>
#include <kernel/proc.h>
#include <asm/cpu.h>
#include <kernel/vdso.h>
#include <kernel/time.h>
#include <kernel/csprng.h>
#include <kernel/ptrace.h>
#include <kernel/errno.h>
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
	if (!uts) return -EFAULT;
	uts_namespace_t *ns = current_uts();
	linux_utsname_t k;
	memset(&k, 0, sizeof(k));
	/* Ranix by name; a process tree that runs software written for Linux
	 * (prctl PR_RANIX_UNAME_LINUX, set by the Linux userland launcher) sees "Linux" */
	if (task_current()->uname_linux) memcpy(k.sysname, "Linux", 6);
	else                              memcpy(k.sysname, "Ranix", 6);
	memcpy(k.nodename,   ns->nodename,   sizeof(k.nodename));
	memcpy(k.release,    "6.6.0-ranix", 12);
	memcpy(k.version,    "#1 SMP",  7);
#if defined(__x86_64__)
	memcpy(k.machine,    "x86_64",  7);
#else
	memcpy(k.machine,    "aarch64", 8);
#endif
	memcpy(k.domainname, ns->domainname, sizeof(k.domainname));
	return copy_to_user(uts, &k, sizeof(k)) ? -14 : 0;
}

/*
 * unshare(2) — detach the caller from namespaces shared with its parent.
 * Only CLONE_NEWUTS is supported: it gives the caller a private copy of the
 * UTS namespace, so later sethostname/uname changes stay local.
 */
static long sys_unshare(unsigned long flags)
{
	process_t *p = task_current();
	if (!may_create_ns(p, flags)) return -EPERM;
	if ((flags & CLONE_FS) && p->fs && p->fs->users > 1) {
		/* a private copy: chdir and chroot no longer reach the others */
		fs_struct_t *fs = fs_copy(p->fs);
		if (!fs) return -ENOMEM;
		fs_put(p->fs);
		p->fs = fs;
	}
	if (flags & CLONE_NEWUTS) {
		uts_namespace_t *ns = uts_ns_clone(p->uts_ns);
		if (!ns) return -ENOMEM;
		uts_ns_put(p->uts_ns);
		p->uts_ns = ns;
	}
	if (flags & CLONE_NEWIPC) {
		ipc_namespace_t *ns = ipc_ns_clone(p->ipc_ns);
		if (!ns) return -ENOMEM;
		ipc_ns_put(p->ipc_ns);
		p->ipc_ns = ns;
	}
	if (flags & CLONE_NEWTIME) {
		/* the new time ns applies to future children, not the caller, so its
		 * offsets can still be set via /proc/self/timens_offsets */
		if (!p->time_ns_new) {
			time_namespace_t *ns = time_ns_clone(p->time_ns);
			if (!ns) return -ENOMEM;
			p->time_ns_new = ns;
		}
	}
	if (flags & CLONE_NEWNS) {
		mnt_namespace_t *ns = mnt_ns_clone(p->mnt_ns);
		if (!ns) return -ENOMEM;
		mnt_ns_put(p->mnt_ns);
		p->mnt_ns = ns;
	}
	if (flags & CLONE_NEWUSER) {
		/* moves the caller into a new user namespace and makes it all-powerful
		 * there (rootless-container basis); uid_map is written afterwards */
		user_namespace_t *ns = user_ns_create_child(p->user_ns ? p->user_ns : &init_user_ns, p->euid);
		if (!ns) return -ENOMEM;
		user_ns_put(p->user_ns);
		p->user_ns = ns;
		p->cap_effective = p->cap_permitted = CAP_FULL;
	}
	if (flags & CLONE_NEWNET) {
		/* moves the caller into a new network namespace (only loopback) */
		net_namespace_t *ns = net_ns_create();
		if (!ns) return -ENOMEM;
		net_ns_put(p->net_ns);
		p->net_ns = ns;
	}
	if (flags & CLONE_NEWCGROUP) {
		/* the caller's current cgroup becomes the root of the new ns's view */
		struct cgroup_namespace *ns = cgroup_ns_create();
		if (!ns) return -ENOMEM;
		cgroup_ns_put(p->cgroup_ns);
		p->cgroup_ns = ns;
	}
	if (flags & CLONE_NEWPID) {
		/* applies to future children, not the caller (the caller's pid does
		 * not change); the first child becomes nr 1 of the new ns */
		if (!p->pid_ns_new) {
			pid_namespace_t *ns = pid_ns_create_child(p->pid_ns ? p->pid_ns : &init_pid_ns);
			if (!ns) return -ENOMEM;
			p->pid_ns_new = ns;
		}
	}
	return 0;
}

/*
 * setns(2) — join the namespace referred to by an open ns fd. Only the UTS
 * namespace is supported; nstype, if given, must match.
 */
static long setns_file(process_t *p, vfs_file_t *f, int nstype)
{
	uts_namespace_t *uts = nsfile_to_uts(f);
	if (uts) {
		if (nstype != 0 && nstype != CLONE_NEWUTS) return -EINVAL;
		if (!capable_task(p, CAP_SYS_ADMIN)) return -EPERM;
		uts_ns_get(uts);
		uts_ns_put(p->uts_ns);
		p->uts_ns = uts;
		return 0;
	}

	ipc_namespace_t *ipc = nsfile_to_ipc(f);
	if (ipc) {
		if (nstype != 0 && nstype != CLONE_NEWIPC) return -EINVAL;
		if (!capable_task(p, CAP_SYS_ADMIN)) return -EPERM;
		ipc_ns_get(ipc);
		ipc_ns_put(p->ipc_ns);
		p->ipc_ns = ipc;
		return 0;
	}

	time_namespace_t *tns = nsfile_to_time(f);
	if (tns) {
		if (nstype != 0 && nstype != CLONE_NEWTIME) return -EINVAL;
		if (!capable_task(p, CAP_SYS_ADMIN)) return -EPERM;
		time_ns_get(tns);
		time_ns_put(p->time_ns);
		p->time_ns = tns;
		return 0;
	}

	mnt_namespace_t *mns = nsfile_to_mnt(f);
	if (mns) {
		if (nstype != 0 && nstype != CLONE_NEWNS) return -EINVAL;
		if (!capable_task(p, CAP_SYS_ADMIN)) return -EPERM;
		mnt_ns_get(mns);
		mnt_ns_put(p->mnt_ns);
		p->mnt_ns = mns;
		return 0;
	}

	pid_namespace_t *pns = nsfile_to_pid(f);
	if (pns) {
		if (nstype != 0 && nstype != CLONE_NEWPID) return -EINVAL;
		if (!capable_task(p, CAP_SYS_ADMIN)) return -EPERM;
		/* like unshare(CLONE_NEWPID): the caller's own pid ns is unchanged;
		 * the joined ns becomes the ns for its future children */
		pid_ns_get(pns);
		if (p->pid_ns_new) pid_ns_put(p->pid_ns_new);
		p->pid_ns_new = pns;
		return 0;
	}

	user_namespace_t *uns = nsfile_to_user(f);
	if (uns) {
		if (nstype != 0 && nstype != CLONE_NEWUSER) return -EINVAL;
		if (!capable_task(p, CAP_SYS_ADMIN)) return -EPERM;
		user_ns_get(uns);
		user_ns_put(p->user_ns);
		p->user_ns = uns;
		return 0;
	}

	net_namespace_t *nns = nsfile_to_net(f);
	if (nns) {
		if (nstype != 0 && nstype != CLONE_NEWNET) return -EINVAL;
		if (!capable_task(p, CAP_SYS_ADMIN)) return -EPERM;
		net_ns_get(nns);
		net_ns_put(p->net_ns);
		p->net_ns = nns;
		return 0;
	}

	struct cgroup_namespace *cns = nsfile_to_cgroup(f);
	if (cns) {
		if (nstype != 0 && nstype != CLONE_NEWCGROUP) return -EINVAL;
		if (!capable_task(p, CAP_SYS_ADMIN)) return -EPERM;
		cgroup_ns_get(cns);
		cgroup_ns_put(p->cgroup_ns);
		p->cgroup_ns = cns;
		return 0;
	}

	return -EINVAL;   /* not a namespace fd */
}

static long sys_setns(int fd, int nstype)
{
	process_t *p = task_current();
	vfs_file_t *f = fget(p->fds, fd);
	if (!f) return -EBADF;
	long r = setns_file(p, f, nstype);
	fput(f);
	return r;
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


/* ── futex: event-driven, per-address hash buckets (futex_q model) ──────
 * Waiters are stack-allocated nodes linked into a bucket keyed by
 * (address space, uaddr), or by the page for a MAP_SHARED one (futex_key).
 * FUTEX_WAIT does an atomic value-check + enqueue under
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
                         void *mm2, uint32_t *uaddr2, int max_req)
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
			q->mm = mm2;
			q->uaddr = uaddr2;
			unsigned h2 = futex_hash(mm2, uaddr2);
			q->next = futex_bucket[h2]; futex_bucket[h2] = q;
			requeued++;
		} else {
			pp = &q->next;
		}
	}
	return woken;
}

static void mono_time(int64_t *sec, int64_t *nsec);
static void real_time(int64_t *sec, int64_t *nsec);

/* Which word a futex is. In a MAP_SHARED mapping it is the same word for every
 * process that maps the page, so it is keyed by the page (mm NULL, physical
 * address), as Linux keys it by inode and offset. Anything else, and every
 * FUTEX_PRIVATE_FLAG futex, belongs to this address space alone. */
static void futex_key(process_t *self, int op, uint32_t *uaddr, void **mm, uint32_t **key)
{
	*mm = self->vm;
	*key = uaddr;
	if ((op & FUTEX_PRIVATE_FLAG) || !uaddr || !self->vm) return;
	uint64_t va = (uint64_t)uaddr;
	vma_t *v = self->vm->vmas;
	while (v && !(va >= v->vaddr && va < v->vaddr + v->size)) v = v->next;
	if (!v || !(v->flags & VMA_SHARED)) return;
	uint32_t cur;
	if (copy_from_user(&cur, uaddr, sizeof(cur)) != 0) return;   /* faults the page in */
	uint64_t pa = arch_vm_get_paddr(self->vm->root, va);   /* a huge page's comes with the offset */
	if (!pa) return;
	*mm = NULL;
	*key = (uint32_t *)((pa & ~(uint64_t)(PAGE_SIZE - 1)) | (va & (PAGE_SIZE - 1)));
}

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

/* The wait's deadline from the user's timeout, as an absolute mono_time in
 * *end_ns; *timed stays 0 for no timeout (or one too far off to matter). */
static long futex_deadline(int op, int cmd, linux_timespec_t *timeout, int64_t *end_ns_out, int *timed_out)
{
	int timed = 0;
	int64_t end_ns = 0;   /* absolute mono_time deadline (ns) */
	if (timeout) {
		linux_timespec_t kts;
		if (copy_from_user(&kts, timeout, sizeof(kts)) != 0) return -EFAULT;
		if (kts.tv_sec < 0 || kts.tv_nsec < 0 || kts.tv_nsec >= 1000000000L) return -EINVAL;
		if (kts.tv_sec > 0x7fffffffffffffffLL / 1000000000LL - 2) timeout = NULL;
		else {
			/* FUTEX_WAIT's timeout is a relative span, FUTEX_WAIT_BITSET's an
			 * absolute deadline on the same boot-relative clock mono_time
			 * reports. Track the deadline in ns and decide the timeout against
			 * mono_time — kernel_ticks does not track wall time under TCG, so
			 * it serves only as the deadline-sweep wake-up hint below. */
			int64_t ns, nn; mono_time(&ns, &nn);
			int64_t now_ns = ns * 1000000000LL + nn;
			int64_t want   = kts.tv_sec * 1000000000LL + kts.tv_nsec;
			end_ns = (cmd == 9) ? want : now_ns + want;
			/* FUTEX_CLOCK_REALTIME: that instant is wall-clock time (glibc's
			 * pthread_cond_timedwait), decades past the boot clock: move it onto
			 * mono_time, or the wait never times out. */
			if (cmd == 9 && (op & FUTEX_CLOCK_REALTIME)) {
				int64_t rs, rn; real_time(&rs, &rn);
				end_ns = want - ((rs * 1000000000LL + rn) - now_ns);
			}
			timed  = 1;
		}
	}
	*end_ns_out = end_ns; *timed_out = timed;
	return 0;
}

/* FUTEX_WAIT and FUTEX_WAIT_BITSET: sleep while *uaddr == val, until woken, a
 * delivered signal, a changed word or the deadline. */
static long futex_wait(process_t *self, void *mm, uint32_t *key, uint32_t *uaddr, int op, int cmd,
                       uint32_t val, linux_timespec_t *timeout)
{
	int timed = 0;
	int64_t end_ns = 0;   /* absolute mono_time deadline (ns) */
	long err = futex_deadline(op, cmd, timeout, &end_ns, &timed);
	if (err) return err;
	if (task_has_actionable_signal()) return -EINTR;   /* the first sleep would not see it */
	futex_q_t q = { self, key, mm, 0, NULL };
	/* Fault the word in before taking the lock: the fault path can sleep and
	 * allocate, which it must not do under an IRQ-off spinlock. */
	uint32_t cur;
	if ((uintptr_t)uaddr & 3) return -EINVAL;
	if (copy_from_user(&cur, uaddr, sizeof(cur)) != 0) return -EFAULT;
	uint64_t f = spin_lock_irq(&futex_lock);
	if (copy_from_user(&cur, uaddr, sizeof(cur)) != 0) { spin_unlock_irq(&futex_lock, f); return -EFAULT; }
	if (cur != val)                                    { spin_unlock_irq(&futex_lock, f); return -EAGAIN; }
	unsigned h = futex_hash(mm, key);
	q.next = futex_bucket[h]; futex_bucket[h] = &q;
	uint64_t fs = spin_lock_irq(&sched_lock);
	self->state = PROC_SLEEPING;
	/* event-driven: woken by futex_do_wake or task_signal. timed waits also
	 * arm wake_deadline so the sched_tick deadline sweep fires a wake-up — no
	 * per-tick blanket poll either way. */
	self->sleeping_on_queue = 1;
	if (timed) { self->wake_deadline = futex_deadline_ticks(end_ns);
	             sched_note_wake_deadline(self->wake_deadline); }
	spin_unlock_irq(&sched_lock, fs);
	spin_unlock_irq(&futex_lock, f);

	for (;;) {
		arch_yield();
		f = spin_lock_irq(&futex_lock);
		if (q.woken) { self->wake_deadline = 0; spin_unlock_irq(&futex_lock, f); return 0; }
		/* Only a signal that will actually be delivered interrupts the wait.
		 * An ignored (SIG_IGN) or non-fatal-default pending signal must not —
		 * a process that ignores SIGPIPE would otherwise get a spurious EINTR
		 * on a condvar wait. Mirrors pipe/event's task_has_actionable_signal. */
		int sig = task_has_actionable_signal();
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
		if (timed) { self->wake_deadline = futex_deadline_ticks(end_ns);
		             sched_note_wake_deadline(self->wake_deadline); }
		spin_unlock_irq(&sched_lock, fs);
		spin_unlock_irq(&futex_lock, f);
	}
}

/* FUTEX_WAKE_OP: change *uaddr2 by the op in val3, wake val waiters on uaddr and,
 * when the old value passes val3's comparison, up to val2 (in timeout) on uaddr2. */
static long futex_wake_op(void *mm, uint32_t *key, uint32_t val, void *mm2, uint32_t *key2,
                          uint32_t *uaddr2, linux_timespec_t *timeout, uint32_t val3)
{
	if (!uaddr2) return -EINVAL;
	uint32_t op3 = (val3 >> 28) & 0xf, cmp3 = (val3 >> 24) & 0xf;
	uint32_t oparg = (val3 >> 12) & 0xfff, cmparg = val3 & 0xfff;
	if (op3 & 8) { oparg = 1u << (oparg & 31); op3 &= ~8u; }
	uint32_t oldval = 0;
	if (copy_from_user(&oldval, uaddr2, sizeof(oldval)) != 0) return -EFAULT;
	uint32_t newval = oldval;
	switch (op3) {
	case 0: newval  = oparg; break; case 1: newval += oparg; break;
	case 2: newval |= oparg; break; case 3: newval &= ~oparg; break;
	case 4: newval ^= oparg; break;
	}
	if (copy_to_user(uaddr2, &newval, sizeof(newval)) != 0) return -EFAULT;
	uint64_t f = spin_lock_irq(&futex_lock);
	int woken = futex_do_wake(mm, key, (int)val, NULL, NULL, 0);
	int do2 = 0;
	switch (cmp3) {
	case 0: do2 = (oldval == cmparg); break; case 1: do2 = (oldval != cmparg); break;
	case 2: do2 = ((int32_t)oldval <  (int32_t)cmparg); break;
	case 3: do2 = ((int32_t)oldval <= (int32_t)cmparg); break;
	case 4: do2 = ((int32_t)oldval >  (int32_t)cmparg); break;
	case 5: do2 = ((int32_t)oldval >= (int32_t)cmparg); break;
	}
	if (do2) woken += futex_do_wake(mm2, key2, (int)(uintptr_t)timeout, NULL, NULL, 0);
	spin_unlock_irq(&futex_lock, f);
	return woken;
}

static long sys_futex(uint32_t *uaddr, int op, uint32_t val,
                      linux_timespec_t *timeout, uint32_t *uaddr2, uint32_t val3)
{
	int cmd = op & FUTEX_CMD_MASK;
	/* Priority inheritance is not here. EOPNOTSUPP is what libraries probe for and
	 * fall back on; ENOSYS from a futex makes PulseAudio's client abort. */
	if (cmd >= 6 && cmd <= 8)
		return -EOPNOTSUPP;
	process_t *self = task_current();
	void *mm, *mm2 = NULL;
	uint32_t *key, *key2 = NULL;
	futex_key(self, op, uaddr, &mm, &key);
	if (cmd == FUTEX_WAKE_OP || cmd == FUTEX_REQUEUE || cmd == 4 /* FUTEX_CMP_REQUEUE */)
		futex_key(self, op, uaddr2, &mm2, &key2);

	if (cmd == FUTEX_WAIT || cmd == 9 /* FUTEX_WAIT_BITSET */)
		return futex_wait(self, mm, key, uaddr, op, cmd, val, timeout);

	if (cmd == FUTEX_WAKE) {
		uint64_t f = spin_lock_irq(&futex_lock);
		int woken = futex_do_wake(mm, key, (int)val, NULL, NULL, 0);
		spin_unlock_irq(&futex_lock, f);
		return woken;
	}

	if (cmd == FUTEX_WAKE_OP)
		return futex_wake_op(mm, key, val, mm2, key2, uaddr2, timeout, val3);

	if (cmd == FUTEX_REQUEUE || cmd == 4 /* FUTEX_CMP_REQUEUE */) {
		if (cmd == 4) {
			uint32_t cur;
			if (copy_from_user(&cur, uaddr, sizeof(cur)) != 0) return -EFAULT;
			if (cur != val3) return -EAGAIN;
		}
		uint64_t f = spin_lock_irq(&futex_lock);
		/* onto itself the requeue keeps re-finding the same waiter, up to 2^31
		 * times with IRQs off */
		int same = mm == mm2 && key == key2;
		int woken = futex_do_wake(mm, key, (int)val, mm2, same ? NULL : key2,
		                          (int)(uintptr_t)timeout);
		spin_unlock_irq(&futex_lock, f);
		return woken;
	}

	return -ENOSYS;
}

/* Called from sched.c task_exit to wake futex waiters on clear_child_tid. */
void futex_wake_addr(uint32_t *uaddr)
{
	if (!uaddr) return;
	process_t *self = task_current();
	void *mm = self ? self->vm : NULL;
	uint32_t *key = uaddr;
	if (self) futex_key(self, 0, uaddr, &mm, &key);
	uint64_t f = spin_lock_irq(&futex_lock);
	futex_do_wake(mm, key, 0x7fffffff, NULL, NULL, 0);
	spin_unlock_irq(&futex_lock, f);
}

/* ── robust futexes (NPTL robust-mutex support) ──────────────────────────
 * A thread registers a singly-linked user-space list of the futexes it owns
 * via set_robust_list. On exit the kernel walks it and, for every futex word
 * still tagged with this tid, sets FUTEX_OWNER_DIED and wakes one waiter so a
 * surviving thread can recover the lock. The list is in the dying thread's own
 * memory, so every access goes through copy_{from,to}_user: a garbage or
 * malicious list must not fault the kernel — the walk is length-bounded and
 * tolerates faults. PI is out of scope. */
#define FUTEX_WAITERS     0x80000000u
#define FUTEX_OWNER_DIED  0x40000000u
#define FUTEX_TID_MASK    0x3fffffffu
#define ROBUST_LIST_LIMIT 2048   /* bounds a cyclic/garbage list */

struct robust_list_head {
	void   *list;             /* first robust_list node, or &head->list if empty */
	long    futex_offset;     /* signed byte offset from a node to its futex word */
	void   *list_op_pending;  /* node mid lock/unlock; not yet on the list */
};

/* Set OWNER_DIED on a futex this dying thread still owns and wake one waiter so
 * a survivor can take over. uaddr is a user pointer derived from the list. */
static void robust_handle_futex(process_t *self, void *mm, uint32_t *uaddr, uint32_t tid)
{
	uint32_t cur;
	for (;;) {
		if (copy_from_user(&cur, uaddr, sizeof(cur)) != 0) return;
		if ((cur & FUTEX_TID_MASK) != tid) return;   /* not ours (or already handed off) */
		uint32_t new = (cur & FUTEX_WAITERS) | FUTEX_OWNER_DIED;
		/* No userspace CAS primitive here: re-read after the store and retry if
		 * it changed, so a racing lock attempt does not get clobbered silently. */
		if (copy_to_user(uaddr, &new, sizeof(new)) != 0) return;
		uint32_t check;
		if (copy_from_user(&check, uaddr, sizeof(check)) != 0) return;
		if (check == new) break;
	}
	uint32_t *key = uaddr;
	futex_key(self, 0, uaddr, &mm, &key);
	uint64_t f = spin_lock_irq(&futex_lock);
	futex_do_wake(mm, key, 1, NULL, NULL, 0);
	spin_unlock_irq(&futex_lock, f);
}

/* Walk this thread's robust list on exit. Called from task_exit while p->vm is
 * still live and outside sched_lock (futex→sched lock order). */
void futex_exit_robust(process_t *p)
{
	if (!p->robust_list) return;
	void *mm = p->vm;
	uint32_t tid = (uint32_t)task_vpid(p) & FUTEX_TID_MASK;

	struct robust_list_head head;
	if (copy_from_user(&head, p->robust_list, sizeof(head)) != 0) return;

	/* list_op_pending names a futex whose lock/unlock the thread was mid-way
	 * through (so it may own it but it is not yet linked); handle it first. */
	if (head.list_op_pending)
		robust_handle_futex(p, mm, (uint32_t *)((char *)head.list_op_pending + head.futex_offset), tid);

	void *node = head.list;
	for (int i = 0; node && node != p->robust_list && i < ROBUST_LIST_LIMIT; i++) {
		if (node != head.list_op_pending)
			robust_handle_futex(p, mm, (uint32_t *)((char *)node + head.futex_offset), tid);
		void *next;
		if (copy_from_user(&next, node, sizeof(next)) != 0) return;
		node = next;
	}
}

static long sys_set_robust_list(void *head, size_t len)
{
	if (len != sizeof(struct robust_list_head)) return -EINVAL;
	task_current()->robust_list = head;
	return 0;
}

static long sys_get_robust_list(pid_t tid, void **head_ptr, size_t *len_ptr)
{
	process_t *t = tid ? proc_get_user(tid) : task_current();
	if (!t) return -ESRCH;
	/* the head is a user address of the target: an ASLR leak without this */
	if (tid && !ptrace_may_access(t)) { proc_put(t); return -1; }
	void  *head = t->robust_list;
	size_t len  = sizeof(struct robust_list_head);
	if (tid) proc_put(t);
	if (copy_to_user(head_ptr, &head, sizeof(head)) != 0) return -EFAULT;
	if (copy_to_user(len_ptr, &len, sizeof(len)) != 0)    return -EFAULT;
	return 0;
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
	uint64_t hns = tsc_now_ns();
	if (!hns) hns = hpet_now_ns();
	if (hns) {
		*sec  = (int64_t)(hns / 1000000000ULL);
		*nsec = (int64_t)(hns % 1000000000ULL);
		return;
	}
#endif
	*sec  = (int64_t)(kernel_ticks / TIMER_HZ);
	*nsec = (int64_t)((kernel_ticks % TIMER_HZ) * (1000000000 / TIMER_HZ));
}

/* Monotonic nanoseconds from the hardware counter — advances regardless of IRQ
 * state, unlike kernel_ticks (only cpu0's timer tick bumps it). A busy-wait in a
 * syscall runs with IRQs masked on aarch64, so a kernel_ticks deadline there can
 * never arrive if it lands on cpu0; mono_ns() lets such loops time out. */
uint64_t mono_ns(void)
{
	int64_t s, n;
	mono_time(&s, &n);
	return (uint64_t)s * 1000000000ULL + (uint64_t)n;
}

#define CLOCK_REALTIME   0
#define CLOCK_MONOTONIC  1
#define CLOCK_BOOTTIME   7

/* realtime = mono_time + g_rt_off (set from the RTC at boot, or settimeofday/
 * clock_settime / NTP later). 0 offset until rtc_init → falls back to uptime. */
static int64_t g_rt_off_sec  = 0;
static int64_t g_rt_off_nsec = 0;
static int vdso_advertise = 1;         /* AT_SYSINFO_EHDR on/off (see execve) */

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
	int64_t e = arch_rtc_read();
	if (e > 1000000000LL) {           /* sane epoch (after 2001) */
		int64_t ms, mn; mono_time(&ms, &mn);
		g_rt_off_sec  = e - ms;
		g_rt_off_nsec = -mn;
	}
}

/* filesystem timestamps: real wall-clock once the RTC set it, else uptime. */
void ranix_walltime(int64_t *sec, int64_t *nsec) { real_time(sec, nsec); }
void ranix_monotime(int64_t *sec, int64_t *nsec) { mono_time(sec, nsec); }

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
	if (!tp) return -EFAULT;
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
		return -EINVAL;   /* unknown clock */
	}
	return copy_to_user(tp, &k, sizeof(k)) ? -14 : 0;
}

static long sys_clock_getres(int clkid, linux_timespec_t *res)
{
	switch (clkid) {
	case CLOCK_REALTIME: case CLOCK_MONOTONIC: case CLOCK_BOOTTIME:
	case CLOCK_MONOTONIC_RAW: case CLOCK_TAI:
	case CLOCK_PROCESS_CPUTIME_ID: case CLOCK_THREAD_CPUTIME_ID:
	case CLOCK_REALTIME_COARSE: case CLOCK_MONOTONIC_COARSE:
		break;
	default: return -EINVAL;
	}
	if (res) {
		linux_timespec_t r;
		/* The resolution of a clock is what its timers can do, as Linux reports
		 * it (hrtimer_resolution): 1 ns with a one-shot timer (hrtimer_on), else
		 * a tick, like a Linux kernel without high resolution timers. Reading
		 * the clock is nanosecond-fine either way. CPU-time clocks: 1 ns. */
		r.tv_sec = 0;
		r.tv_nsec = (clkid == CLOCK_PROCESS_CPUTIME_ID || clkid == CLOCK_THREAD_CPUTIME_ID || hrtimer_on)
		          ? 1 : 1000000000L / TIMER_HZ;
		return copy_to_user(res, &r, sizeof(r)) ? -14 : 0;
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
	return copy_to_user(tv, &k, sizeof(k)) ? -14 : 0;
}

static long sys_clock_settime(int clkid, const linux_timespec_t *tp)
{
	if (!capable_task(task_current(), CAP_SYS_TIME)) return -EPERM;
	if (clkid != CLOCK_REALTIME || !tp) return -EINVAL;
	linux_timespec_t k;
	if (copy_from_user(&k, tp, sizeof(k)) != 0) return -EFAULT;
	if (k.tv_sec < 0 || k.tv_nsec < 0 || k.tv_nsec >= 1000000000L) return -EINVAL;
	int64_t ms, mn; mono_time(&ms, &mn);
	g_rt_off_sec  = (int64_t)k.tv_sec  - ms;
	g_rt_off_nsec = (int64_t)k.tv_nsec - mn;
	vdso_time_changed();
	return 0;
}

/* persist the current wall-clock to the hardware RTC (hwclock --systohc). */
static void rtc_sync_to_hw(void)
{
	int64_t s, n; real_time(&s, &n);
	arch_rtc_write(s);
}

static long sys_settimeofday(const linux_timeval_t *tv, const void *tz)
{
	(void)tz;
	if (!capable_task(task_current(), CAP_SYS_TIME)) return -EPERM;
	if (!tv) return -EFAULT;
	linux_timeval_t k;
	if (copy_from_user(&k, tv, sizeof(k)) != 0) return -EFAULT;
	if (k.tv_sec < 0 || k.tv_usec < 0 || k.tv_usec >= 1000000L) return -EINVAL;
	int64_t ms, mn; mono_time(&ms, &mn);
	g_rt_off_sec  = (int64_t)k.tv_sec - ms;
	g_rt_off_nsec = (int64_t)k.tv_usec * 1000 - mn;
	vdso_time_changed();
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
	if (!utx) return -EFAULT;
	k_timex_t tx;
	if (copy_from_user(&tx, utx, sizeof(tx)) != 0) return -EFAULT;
	if (tx.modes != 0 && !capable_task(task_current(), CAP_SYS_TIME))
		return -EPERM;   /* read-only adjtimex (modes==0) stays unprivileged */

	if (tx.modes & 0x0100) {           /* ADJ_SETOFFSET: step the clock by .time */
		int64_t add_s = tx.time_sec;
		int64_t add_n = (tx.modes & 0x2000) ? tx.time_usec   /* ADJ_NANO */
		                                     : tx.time_usec * 1000;
		int64_t ms, mn, cs, cn; mono_time(&ms, &mn); real_time(&cs, &cn);
		int64_t ns = cs + add_s + add_n / 1000000000LL, nn = cn + add_n % 1000000000LL;
		if (nn >= 1000000000LL) { nn -= 1000000000LL; ns++; }
		if (nn < 0)             { nn += 1000000000LL; ns--; }
		g_rt_off_sec = ns - ms; g_rt_off_nsec = nn - mn;
		vdso_time_changed();
		rtc_sync_to_hw();
	}
	if (tx.modes & 0x0002) g_tx_freq = tx.freq;   /* ADJ_FREQUENCY: store steer */

	/* report current discipline state */
	tx.offset = 0; tx.freq = g_tx_freq; tx.maxerror = 0; tx.esterror = 0;
	tx.status = 0; tx.tick = 10000; tx.tai = 0;
	return copy_to_user(utx, &tx, sizeof(tx)) ? -14 : 0;   /* TIME_OK */
}

/* EINTR with the time still to sleep in *urem, so a retry loop resumes */
static long sleep_eintr(uint64_t left_ns, linux_timespec_t *urem)
{
	if (urem) {
		linux_timespec_t r;
		r.tv_sec  = (long)(left_ns / 1000000000ULL);
		r.tv_nsec = (long)(left_ns % 1000000000ULL);
		if (copy_to_user(urem, &r, sizeof(r)) != 0) return -EFAULT;
	}
	return -EINTR;
}

/* Never shorter than asked (POSIX), and with a one-shot timer not much longer:
 * the sleep is to an instant on CLOCK_MONOTONIC (task_sleep_until_ns), which
 * the CPU's timer is set for; without one it ends on the tick after it. */
static long sleep_for(const linux_timespec_t *kts, linux_timespec_t *urem)
{
	linux_timespec_t k = *kts;
	if (k.tv_sec < 0 || k.tv_nsec < 0 || k.tv_nsec >= 1000000000L) return -EINVAL;
	/* a huge span must saturate: a wrapped end returns at once */
	int huge = (uint64_t)k.tv_sec >= (uint64_t)-1 / 2 / 1000000000ULL;
	uint64_t want = huge ? (uint64_t)-1 / 2 : (uint64_t)k.tv_sec * 1000000000ULL + (uint64_t)k.tv_nsec;
	uint64_t end = mono_ns() + want;
	for (;;) {
		uint64_t now = mono_ns();
		if (now >= end) return 0;
		if (task_has_actionable_signal())   /* EINTR (delivered signal only) */
			return sleep_eintr(end - now, urem);
		task_sleep_until_ns(end);
	}
}

static long sys_nanosleep(linux_timespec_t *req, linux_timespec_t *rem)
{
	if (!req) return -EFAULT;
	linux_timespec_t k;
	if (copy_from_user(&k, req, sizeof(k)) != 0) return -EFAULT;
	return sleep_for(&k, rem);
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
    default: return -EINVAL;
    }
    uint64_t hz = TIMER_HZ;
    if (old_v) {
        itimerval_t oval;
        oval.it_value.tv_sec    = (int64_t)(*value / hz);
        oval.it_value.tv_usec   = (int64_t)((*value % hz) * (1000000 / hz));
        oval.it_interval.tv_sec = (int64_t)(*interval / hz);
        oval.it_interval.tv_usec= (int64_t)((*interval % hz) * (1000000 / hz));
        if (copy_to_user(old_v, &oval, sizeof(oval)) != 0) return -EFAULT;
    }
    if (new_v) {
        itimerval_t nval;
        if (copy_from_user(&nval, new_v, sizeof(nval)) != 0) return -EFAULT;
        if (nval.it_value.tv_sec < 0 || nval.it_interval.tv_sec < 0 ||
            (uint64_t)nval.it_value.tv_usec >= 1000000 ||
            (uint64_t)nval.it_interval.tv_usec >= 1000000) return -EINVAL;
        uint64_t nv = (uint64_t)nval.it_value.tv_sec * hz +
                      (uint64_t)nval.it_value.tv_usec * hz / 1000000;
        uint64_t ni = (uint64_t)nval.it_interval.tv_sec * hz +
                      (uint64_t)nval.it_interval.tv_usec * hz / 1000000;
        /* store both fields under sched_lock so a nested timer tick (aarch64) or
         * a cross-CPU sched_tick can't decrement against a half-updated pair. */
        uint64_t sf = spin_lock_irq(&sched_lock);
        if (which == 0) sched_itimer_account(*value, nv);   /* ITIMER_REAL gate */
        *value    = nv;
        *interval = ni;
        spin_unlock_irq(&sched_lock, sf);
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
		tms[0] = (int64_t)TICKS_USER(p ? p->utime_ticks  : 0);
		tms[1] = (int64_t)TICKS_USER(p ? p->stime_ticks  : 0);
		tms[2] = (int64_t)TICKS_USER(p ? p->cutime_ticks : 0);
		tms[3] = (int64_t)TICKS_USER(p ? p->cstime_ticks : 0);
		if (copy_to_user(buf, tms, sizeof(tms)) != 0) return -EFAULT;
	}
	return (long)TICKS_USER(kernel_ticks);
}

static long sys_sysinfo(void *info)
{
	if (!info) return -EFAULT;
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
	return copy_to_user(info, &si, sizeof(si)) ? -14 : 0;
}

/* ── execve ──────────────────────────────────────────────────────────────────
 * Replace the running process image: read and validate the ELF, build a fresh
 * address space (stack + guard page + loaded segments), apply suid/sgid and the
 * capability transition, lay out the SysV initial stack (argv/envp/auxv), then
 * switch to the new space — freeing the old one only after the switch succeeds.
 * Relies on syscall.c helpers (vma_add, rng_next) and the
 * USER_STACK_/USER_MMAP_ macros, all in scope via the textual include.
 * ───────────────────────────────────────────────────────────────────────── */
static long sys_execve(const char *path, char * const argv[], char * const envp[]);
static long do_execve(char *kpath, char * const argv[], char * const envp[]);

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
	if (flags & ~0x800u) return -EINVAL;   /* PIDFD_NONBLOCK */
	process_t *t = pid > 0 ? proc_get_user(pid) : NULL;
	if (!t) return -ESRCH;
	pid_t global = (pid_t)t->pid;   /* the fd can reach another pid namespace */
	proc_put(t);
	vfs_inode_t *in = kmalloc(sizeof(*in));
	if (!in) return -ENOMEM;
	memset(in, 0, sizeof(*in));
	in->fops     = &pidfd_fops;
	in->priv     = (void *)(uint64_t)global;
	in->refcount = 1;
	vfs_file_t *f = kmalloc(sizeof(*f));
	if (!f) { kfree(in); return -ENOMEM; }
	memset(f, 0, sizeof(*f));
	f->inode    = in;
	f->refcount = 0;   /* fd_alloc takes the +1 */
	int fd = fd_alloc(task_current()->fds, f);
	if (fd < 0) { kfree(f); kfree(in); return fd == -1 ? -12 : fd; }
	return fd;
}

static long sys_pidfd_send_signal(int pidfd, int sig, void *info, unsigned int flags)
{
	(void)info;
	if (flags) return -EINVAL;
	vfs_file_t *f = fget(task_current()->fds, pidfd);
	if (!f) return -EBADF;
	if (!f->inode || f->inode->fops != &pidfd_fops) { fput(f); return -EBADF; }
	process_t *t = proc_get((pid_t)(uint64_t)f->inode->priv);
	fput(f);
	long r = (t && task_pid_nr_ns(t, current_pid_ns())) ? kill_task(t, sig) : -3;
	proc_put(t);
	return r;
}

/* ── real-time scheduling policy (SCHED_FIFO/RR) ─────────────────────────────
 * Policy + priority stored per process; cpu_rq_add runs real-time tasks ahead
 * of SCHED_NORMAL ones. */
struct ranix_sched_param { int sched_priority; };

static long sys_sched_setscheduler(pid_t pid, int policy, const void *uparam)
{
	if (policy != SCHED_NORMAL && policy != SCHED_FIFO && policy != SCHED_RR) return -EINVAL;
	/* a real-time spinner starves every normal task on its CPU */
	if (policy != SCHED_NORMAL && !capable_task(task_current(), CAP_SYS_NICE)) return -1;
	process_t *p = pid ? proc_get_user(pid) : task_current();
	if (!p) return -ESRCH;
	if (pid && !task_may_control(p, CAP_SYS_NICE)) { proc_put(p); return -1; } /* EPERM */
	long ret = 0;
	int prio = 0;
	if (uparam) {
		struct ranix_sched_param sp;
		if (copy_from_user(&sp, uparam, sizeof(sp)) != 0) ret = -14;
		else prio = sp.sched_priority;
	}
	if (ret == 0) {
		if (policy == SCHED_NORMAL) { if (prio != 0) ret = -22; }
		else if (prio < 1 || prio > 99) ret = -22;
	}
	if (ret == 0) { p->sched_policy = policy; p->rt_priority = prio; }
	if (pid) proc_put(p);
	return ret;
}

static long sys_sched_setparam(pid_t pid, const void *uparam)
{
	process_t *p = pid ? proc_get_user(pid) : task_current();
	if (!p) return -ESRCH;
	if (pid && !task_may_control(p, CAP_SYS_NICE)) { proc_put(p); return -1; } /* EPERM */
	long ret = 0;
	struct ranix_sched_param sp;
	if (!uparam) ret = -14;
	else if (copy_from_user(&sp, uparam, sizeof(sp)) != 0) ret = -14;
	else if (p->sched_policy == SCHED_NORMAL) { if (sp.sched_priority != 0) ret = -22; }
	else if (sp.sched_priority < 1 || sp.sched_priority > 99) ret = -22;
	else if (sp.sched_priority > p->rt_priority && !capable_task(task_current(), CAP_SYS_NICE)) ret = -1;
	if (ret == 0) p->rt_priority = sp.sched_priority;
	if (pid) proc_put(p);
	return ret;
}

static long sys_sched_getscheduler(pid_t pid)
{
	process_t *p = pid ? proc_get_user(pid) : task_current();
	long r = p ? (long)p->sched_policy : -3;
	if (pid) proc_put(p);
	return r;
}

/* sched_attr, as far as Ranix keeps it: policy, nice, rt priority. Batch and idle
 * are accepted as normal; deadline scheduling is not offered. */
struct ranix_sched_attr {
	uint32_t size, policy;
	uint64_t flags;
	int32_t  nice;
	uint32_t priority;
	uint64_t runtime, deadline, period;
	uint32_t util_min, util_max;
};

static long sys_sched_getattr(pid_t pid, void *uattr, unsigned int usize, unsigned int flags)
{
	if (flags || !uattr || usize < 48 || usize > 4096) return -EINVAL;
	process_t *p = pid ? proc_get_user(pid) : task_current();
	if (!p) return -ESRCH;
	struct ranix_sched_attr a;
	memset(&a, 0, sizeof(a));
	a.size     = usize < sizeof(a) ? usize : (uint32_t)sizeof(a);
	a.policy   = (uint32_t)p->sched_policy;
	a.nice     = p->nice;
	a.priority = (uint32_t)p->rt_priority;
	if (pid) proc_put(p);
	return copy_to_user(uattr, &a, a.size) ? -14 : 0;
}

static long sys_sched_setattr(pid_t pid, const void *uattr, unsigned int flags)
{
	if (flags || !uattr) return -EINVAL;
	struct ranix_sched_attr a;
	memset(&a, 0, sizeof(a));
	uint32_t sz;
	if (copy_from_user(&sz, uattr, 4)) return -EFAULT;
	if (!sz) sz = 48;
	if (sz < 48 || sz > 4096) return -E2BIG;   /* as Linux */
	if (copy_from_user(&a, uattr, sz < sizeof(a) ? sz : sizeof(a))) return -EFAULT;
	int policy = (int)a.policy;
	if (policy == 3 || policy == 5) policy = SCHED_NORMAL;      /* BATCH, IDLE */
	if (policy != SCHED_NORMAL && policy != SCHED_FIFO && policy != SCHED_RR) return -EINVAL;
	if (policy == SCHED_NORMAL) {
		if (a.priority) return -EINVAL;
		long r = sys_sched_setscheduler(pid, SCHED_NORMAL, NULL);
		if (r) return r;
		process_t *p = pid ? proc_get_user(pid) : task_current();
		if (!p) return -ESRCH;
		int nice = a.nice < -20 ? -20 : a.nice > 19 ? 19 : a.nice;
		if (nice < p->nice && !capable_task(task_current(), CAP_SYS_NICE)) r = -1;
		else p->nice = nice;
		if (pid) proc_put(p);
		return r;
	}
	struct ranix_sched_param sp = { (int)a.priority };
	/* sys_sched_setscheduler copies from user memory: hand it the value through a
	 * kernel-side path instead */
	process_t *p = pid ? proc_get_user(pid) : task_current();
	if (!p) return -ESRCH;
	long r = 0;
	if (!capable_task(task_current(), CAP_SYS_NICE)) r = -1;
	else if (pid && !task_may_control(p, CAP_SYS_NICE)) r = -1;
	else if (sp.sched_priority < 1 || sp.sched_priority > 99) r = -22;
	else { p->sched_policy = policy; p->rt_priority = sp.sched_priority; }
	if (pid) proc_put(p);
	return r;
}

/* Are two processes' resources the same object? 0 yes, 1/2 an order, 3 not
 * comparable. File, memory, file table and signal handlers are known here. */
static long sys_kcmp(pid_t pid1, pid_t pid2, int type, unsigned long idx1, unsigned long idx2)
{
	process_t *a = proc_get_user(pid1), *b = proc_get_user(pid2);
	long r = 0;
	if (!a || !b) { r = -3; goto out; }
	if (!task_may_control(a, CAP_SYS_PTRACE) || !task_may_control(b, CAP_SYS_PTRACE)) { r = -1; goto out; }
	const void *x, *y;
	switch (type) {
	case 0: {                                   /* KCMP_FILE */
		vfs_file_t *fa = fget(a->fds, (int)idx1), *fb = fget(b->fds, (int)idx2);
		x = fa; y = fb;
		if (fa) fput(fa);
		if (fb) fput(fb);
		if (!x || !y) { r = -9; goto out; }
		break;
	}
	case 1: x = a->vm; y = b->vm; break;        /* KCMP_VM */
	case 2: x = a->fds; y = b->fds; break;      /* KCMP_FILES */
	case 4: x = a->sighand; y = b->sighand; break;   /* KCMP_SIGHAND */
	default: r = -22; goto out;
	}
	r = x == y ? 0 : (uintptr_t)x < (uintptr_t)y ? 1 : 2;
out:
	proc_put(a);
	proc_put(b);
	return r;
}

/* The memory of a process being killed goes back as it exits; nothing to hurry. */
static long sys_process_mrelease(int pidfd, unsigned int flags)
{
	if (flags) return -EINVAL;
	vfs_file_t *f = fget(task_current()->fds, pidfd);
	if (!f) return -EBADF;
	if (!f->inode || f->inode->fops != &pidfd_fops) { fput(f); return -EBADF; }
	process_t *t = proc_get((pid_t)(uint64_t)f->inode->priv);
	fput(f);
	if (!t) return -ESRCH;
	long r = (t->sig_pending & (1UL << (SIGKILL - 1))) || t->state == PROC_ZOMBIE || t->state == PROC_DEAD ? 0 : -22;
	proc_put(t);
	return r;
}

static long sys_sched_getparam(pid_t pid, void *uparam)
{
	process_t *p = pid ? proc_get_user(pid) : task_current();
	if (!p) return -ESRCH;
	struct ranix_sched_param sp = { p->rt_priority };
	if (pid) proc_put(p);
	if (copy_to_user(uparam, &sp, sizeof(sp)) != 0) return -EFAULT;
	return 0;
}

static long sys_sched_get_priority_max(int policy)
{
	if (policy == SCHED_FIFO || policy == SCHED_RR) return 99;
	if (policy == SCHED_NORMAL) return 0;
	return -EINVAL;
}

static long sys_sched_get_priority_min(int policy)
{
	if (policy == SCHED_FIFO || policy == SCHED_RR) return 1;
	if (policy == SCHED_NORMAL) return 0;
	return -EINVAL;
}

/* execveat(dirfd, path, argv, envp, flags): resolve the dirfd-relative (or
 * AT_EMPTY_PATH) path to an absolute one, then run the normal execve. */
static long sys_execveat(int dirfd, const char *path, char * const argv[],
                         char * const envp[], int flags)
{
	char first = 0;
	if (path && get_user(first, path) != 0) return -EFAULT;
	char *kp = kmalloc(PATH_MAX);
	if (!kp) return -ENOMEM;
	if ((flags & AT_EMPTY_PATH) && !first) {
		vfs_file_t *f = fget(task_current()->fds, dirfd);
		if (!f) { kfree(kp); return -EBADF; }
		if (!f->path[0]) { fput(f); kfree(kp); return -EBADF; }
		memcpy(kp, f->path, strlen(f->path) + 1);
		fput(f);
		return do_execve(kp, argv, envp);
	}
	if (!path) { kfree(kp); return -EFAULT; }
	int r = resolve_path(kp, dirfd, path);
	if (r) { kfree(kp); return r; }
	return do_execve(kp, argv, envp);
}

/* x-permission against the caller's effective ids (execve EACCES check). */
static int exec_permitted(const vfs_stat_t *st, process_t *p)
{
	if (!S_ISREG(st->mode)) return 0;
	mode_t m = st->mode;
	if (p->euid == 0) return (m & 0111) != 0;   /* root: any x bit suffices */
	if (st->uid == p->euid) return (m & 0100) != 0;
	if (st->gid == p->egid) return (m & 0010) != 0;
	return (m & 0001) != 0;
}

#define SHEBANG_MAX_DEPTH 4
#define SHEBANG_LINE_MAX  127

static long sys_execve(const char *path, char * const argv[], char * const envp[])
{
	char *kp = kmalloc(PATH_MAX);
	if (!kp) return -ENOMEM;
	int r = copy_path_from_user(kp, path);
	if (r) { kfree(kp); return r; }
	return do_execve(kp, argv, envp);
}

/* One user string into the staging area at *koff. -EFAULT on a bad pointer,
 * -E2BIG if it overruns the budget. */
static long exec_stage_ustr(char *kstrs, int *koff, int budget, const char *u)
{
	int room = budget - *koff;
	if (room <= 0) return -E2BIG;
	long n = strncpy_from_user(kstrs + *koff, u, (size_t)room);
	if (n < 0) return -EFAULT;
	if (n >= room) return -E2BIG;
	*koff += (int)n + 1;
	return 0;
}

/* Count a user pointer array up to its NULL. An array running past max is left
 * for the budget checks to refuse. */
static long exec_count_uvec(char * const *uv, int max)
{
	int n = 0;
	while (uv && n < max) {
		char *s;
		if (get_user(s, &uv[n]) != 0) return -EFAULT;
		if (!s) break;
		n++;
	}
	return n;
}

/* ── execve, in steps ────────────────────────────────────────────────────────
 * do_execve runs these in order: make the path absolute, follow #! lines, copy
 * argv/envp into the kernel, work out the new image's ids, load the ELF (and
 * its ld.so), map the stack, then past the point of no return switch to the
 * new address space and build the initial stack there. */

/* Staged argv/envp: the strings live in a heap budget (ARG_STR_MAX); the
 * pointer arrays are heap-sized to the actual count. Over-budget strings or
 * counts return -E2BIG (Linux). The staged user stack (strings + pointer arrays
 * + auxv) is later published into the top ARG_WIN_PAGES pages of the new
 * stack, so the whole block must fit that window. */
#define ARG_STR_MAX   (128 * 1024)
#define ARG_WIN_PAGES 64                          /* 256 KB arg window */
#define ARG_WIN_BYTES ((size_t)ARG_WIN_PAGES * PAGE_SIZE)
#define ARG_BASE      (USER_STACK_TOP_ - ARG_WIN_BYTES)
/* the kbuf byte that will land at user address va */
#define KB(va) (kbuf + (((va) - ARG_BASE) & (ARG_WIN_BYTES - 1)))

typedef struct {
	char  *kstrs;       /* argv then envp strings, each NUL-terminated */
	char **kargv;
	char **kenvp;
	int    argc, envc;
	int    koff;        /* bytes used in kstrs */
	int    argv_bytes;  /* the argv part: the /proc/<pid>/cmdline layout */
} exec_args_t;

static void exec_args_free(exec_args_t *a)
{
	kfree(a->kstrs); kfree(a->kargv); kfree(a->kenvp);
}

/* The VFS walks absolute paths only: put the working directory in front of a
 * relative path. -ENAMETOOLONG if it does not fit. */
static int exec_make_absolute(process_t *p, char *kpath)
{
	int i = strlen(kpath);
	if (i && kpath[0] != '/') {
		fs_path_t *cwd = fs_cwd_get(p->fs);
		int bl = strlen(cwd->s);
		if (bl + 1 + i >= PATH_MAX) { fs_path_put(cwd); return -ENAMETOOLONG; }
		memmove(kpath + bl + 1, kpath, i + 1);
		memcpy(kpath, cwd->s, bl);
		kpath[bl] = '/';
		fs_path_put(cwd);
	}
	return 0;
}

/* Resolve #! interpreters and enforce execute permission. Each script level
 * prepends [interp, optarg?, scriptpath] to pre[] and the original argv[0] is
 * dropped. The shebang line is truncated at 127 bytes; nested up to
 * SHEBANG_MAX_DEPTH levels. pre[] entries are kmalloc'd kernel strings. On
 * success *kpathp is the image to load; on failure everything is freed. */
static long exec_resolve_shebang(process_t *p, char **kpathp, char **pre, int *pre_n)
{
	char *kpath = *kpathp;
	int shebang_n = 0;
#define SHEBANG_FAIL(code) do { \
		for (int _k = 0; _k < shebang_n; _k++) kfree(pre[_k]); \
		kfree(kpath); return (code); } while (0)
	for (int depth = 0; ; depth++) {
		vfs_stat_t st = {0};
		if (vfs_stat(kpath, &st) != 0) SHEBANG_FAIL(-2);
		if (!exec_permitted(&st, p)) SHEBANG_FAIL(-13);   /* EACCES */
		char hdr[SHEBANG_LINE_MAX + 1];
		int fd = vfs_open(kpath, O_RDONLY, 0);
		if (fd < 0) SHEBANG_FAIL(-2);
		long got = vfs_read(fd, hdr, sizeof(hdr) - 1);
		vfs_close(fd);
		if (got < 2 || hdr[0] != '#' || hdr[1] != '!') break;   /* not a script */
		if (depth + 1 >= SHEBANG_MAX_DEPTH) SHEBANG_FAIL(-40);   /* ELOOP */

		int hn = (int)got;
		hdr[hn] = '\0';
		for (int e = 0; e < hn; e++) if (hdr[e] == '\n') { hdr[e] = '\0'; break; }

		int s2 = 2; while (hdr[s2] == ' ' || hdr[s2] == '\t') s2++;
		int ie = s2; while (hdr[ie] && hdr[ie] != ' ' && hdr[ie] != '\t') ie++;
		if (ie == s2) SHEBANG_FAIL(-8);   /* ENOEXEC: empty interpreter */

		int as = ie, ae = ie;
		while (hdr[as] == ' ' || hdr[as] == '\t') as++;
		ae = as; while (hdr[ae]) ae++;
		int has_arg = (ae > as);
		hdr[ie] = '\0';   /* terminate interpreter (must come after scanning past it) */

		char *interp = kmalloc(PATH_MAX);
		if (!interp) SHEBANG_FAIL(-12);
		int c = 0; for (; hdr[s2 + c]; c++) interp[c] = hdr[s2 + c];
		interp[c] = '\0';
		pre[shebang_n++] = interp;

		if (has_arg) {
			char *opt = kmalloc((size_t)(ae - as) + 1);
			if (!opt) { kfree(interp); shebang_n--; SHEBANG_FAIL(-12); }
			int oc = 0; for (; as + oc < ae; oc++) opt[oc] = hdr[as + oc];
			opt[oc] = '\0';
			pre[shebang_n++] = opt;
		}
		pre[shebang_n++] = kpath;   /* script path becomes next positional */

		kpath = kmalloc(PATH_MAX);
		if (!kpath) { /* interp string still freeable via pre */
			for (int _k = 0; _k < shebang_n; _k++) kfree(pre[_k]);
			return -ENOMEM;
		}
		c = 0; for (; interp[c] && c < PATH_MAX - 1; c++) kpath[c] = interp[c];
		kpath[c] = '\0';
	}
#undef SHEBANG_FAIL
	*kpathp = kpath;
	*pre_n = shebang_n;
	return 0;
}

/* Stage argv/envp into kernel memory before the VM switch: pre[] (consumed
 * here, freed on every path) then the caller's argv, skipping its argv[0] when
 * a #! prefix replaces it. On failure nothing is left allocated in *a. */
static long exec_stage_args(exec_args_t *a, char **pre, int shebang_n,
                            char * const argv[], char * const envp[])
{
	memset(a, 0, sizeof(*a));
	a->kstrs = kmalloc(ARG_STR_MAX);
	if (!a->kstrs) { for (int k=0;k<shebang_n;k++) kfree(pre[k]); return -ENOMEM; }

	/* count args/env (bounded so an unterminated user array can't loop forever) */
	int max_ptrs = (int)(ARG_WIN_BYTES / sizeof(uint64_t));
	/* a shebang prefix replaces argv[0]; skip the caller's argv[0] in that case */
	int user_skip = shebang_n ? 1 : 0;
	long user_argc = exec_count_uvec(argv, max_ptrs);
	long ucount_env = exec_count_uvec(envp, max_ptrs);
	if (user_argc < 0 || ucount_env < 0) {
		for (int k=0;k<shebang_n;k++) kfree(pre[k]);
		exec_args_free(a); return -EFAULT;
	}
	int user_start = (user_skip && user_argc > 0) ? 1 : 0;
	int argc = shebang_n + ((int)user_argc - user_start);
	int envc = (int)ucount_env;

	a->kargv = kmalloc((size_t)(argc + 1) * sizeof(char *));
	a->kenvp = kmalloc((size_t)(envc + 1) * sizeof(char *));
	if (!a->kargv || !a->kenvp) { for (int k=0;k<shebang_n;k++) kfree(pre[k]); exec_args_free(a); return -ENOMEM; }

	char *kstrs = a->kstrs;
	int koff = 0;
	for (int i = 0; i < argc; i++) {
		/* pre[] is kernel; argv[] and the strings it points at are user,
		 * and a sibling thread may change them under this second read. */
		int at = koff;
		long sr = -7;
		if (i < shebang_n) {
			int n = strlen(pre[i]);
			if (koff + n + 1 <= ARG_STR_MAX) {
				memcpy(kstrs + koff, pre[i], (size_t)n + 1);
				koff += n + 1;
				sr = 0;
			}
		} else {
			char *s = NULL;
			sr = get_user(s, &argv[user_start + (i - shebang_n)]) != 0 || !s ? -14
			   : exec_stage_ustr(kstrs, &koff, ARG_STR_MAX, s);
		}
		if (sr) {
			for (int k=0;k<shebang_n;k++) kfree(pre[k]);
			exec_args_free(a); return sr;
		}
		a->kargv[i] = kstrs + at;
	}
	for (int k = 0; k < shebang_n; k++) kfree(pre[k]);
	/* kstrs[0..koff) now holds the argv strings, each NUL-terminated and
	 * contiguous — exactly the /proc/<pid>/cmdline layout. Capture before envp
	 * strings get appended below. */
	a->argv_bytes = koff;
	for (int e = 0; e < envc; e++) {
		int at = koff;
		char *s = NULL;
		long sr = get_user(s, &envp[e]) != 0 || !s ? -14
		        : exec_stage_ustr(kstrs, &koff, ARG_STR_MAX, s);
		if (sr) { exec_args_free(a); return sr; }
		a->kenvp[e] = kstrs + at;
	}
	a->argc = argc;
	a->envc = envc;
	a->koff = koff;
	return 0;
}

/* The new image's effective ids. no_new_privs (and a still-traced task)
 * suppresses S_ISUID/S_ISGID elevation: the image runs with the caller's ids.
 * A nosuid mount suppresses it too. So does a file owner with no id in the
 * caller's user namespace (Linux bprm_fill_uid): a setuid-root binary run
 * inside a user namespace must not give root of the machine. And so does a
 * root and cwd shared (CLONE_FS) with a task outside the thread group. */
static void exec_new_creds(process_t *p, const vfs_stat_t *st, unsigned long mnt_flags,
                           uid_t *euid, gid_t *egid)
{
	*euid = p->euid;
	*egid = p->egid;
	int nnp = p->no_new_privs || p->tracer != 0 || (mnt_flags & MS_NOSUID);
	user_namespace_t *ns = p->user_ns ? p->user_ns : &init_user_ns;
	if (!kuid_has_mapping(ns, st->uid) || !kgid_has_mapping(ns, st->gid)) nnp = 1;
	if ((st->mode & (S_ISUID | S_ISGID)) && fs_shared_outside_group(p)) nnp = 1;
	if (!nnp && (st->mode & S_ISUID)) *euid = st->uid;
	if (!nnp && (st->mode & S_ISGID)) *egid = st->gid;
}

/* secure-exec env scrub: a privilege-raising exec must not inherit the
 * caller's loader-controlling variables, or a suid binary runs attacker code
 * via ld.so. Drop them from kenvp[] in place; returns the new count. */
static int exec_scrub_env(char **kenvp, int envc)
{
	static const char *const unsafe[] = {
		"LD_PRELOAD=", "LD_LIBRARY_PATH=", "LD_AUDIT=", "LD_ORIGIN_PATH=",
		"LD_PROFILE=", "LD_DEBUG=", "LD_DYNAMIC_WEAK=", "LD_SHOW_AUXV=",
		"GCONV_PATH=", "GETCONF_DIR=", "TMPDIR=", 0 };
	int w = 0;
	for (int e = 0; e < envc; e++) {
		int drop = 0;
		for (int u = 0; unsafe[u]; u++) {
			const char *pfx = unsafe[u]; const char *s = kenvp[e];
			int i = 0; while (pfx[i] && pfx[i] == s[i]) i++;
			if (!pfx[i]) { drop = 1; break; }
		}
		if (!drop) kenvp[w++] = kenvp[e];
	}
	kenvp[w] = 0;
	return w;
}

/* /proc/<pid>/exe names the file itself, symlinks followed (Linux d_path):
 * a program run as /proc/self/exe, as Chromium starts its helpers, must
 * still find its $ORIGIN libraries next to the real binary. */
static void exec_real_path(process_t *p, int fd, char exe_real[PROC_NAME_MAX])
{
	exe_real[0] = '\0';
	vfs_file_t *ef = fd_get(p->fds, fd);
	const char *rp = ef ? (ef->lpath ? ef->lpath : ef->path) : NULL;
	size_t rl = rp ? strlen(rp) : 0;
	if (rp && rp[0] == '/' && rl < PROC_NAME_MAX)
		memcpy(exe_real, rp, rl + 1);
}

/* Where the loaded image starts and what auxv says about it. */
typedef struct {
	uint64_t jump_entry;              /* where the kernel transfers control */
	uint64_t at_base;                 /* AT_BASE: dynamic linker load base */
	uint64_t interp_lo, interp_hi;
	uint64_t mmap_base;
} exec_entry_t;

/* Dynamically linked: load the interpreter (ld.so) too and enter it instead of
 * the executable. It performs relocations in userspace, then jumps to the
 * executable's entry (passed via AT_ENTRY). The caller destroys new_vm on
 * failure. */
static long exec_load_interp(process_t *p, vm_space_t *new_vm, const elf_info_t *ei,
                             exec_entry_t *en)
{
	/* A relative PT_INTERP is relative to the working directory, as on
	 * Linux: VS Code's C/C++ server ships its own musl as "./libc.so" and
	 * is started from its directory. The VFS walks absolute paths only. */
	const char *ipath = ei->interp;
	char *iabs = NULL;
	if (ei->interp[0] != '/') {
		fs_path_t *cwd = fs_cwd_get(p->fs);
		size_t rl = strlen(ei->interp), cl = strlen(cwd->s);
		int slash = cl && cwd->s[cl - 1] != '/';
		iabs = cl + (size_t)slash + rl + 1 > PATH_MAX ? NULL : kmalloc(PATH_MAX);
		if (iabs) {
			memcpy(iabs, cwd->s, cl);
			if (slash) iabs[cl] = '/';
			memcpy(iabs + cl + slash, ei->interp, rl + 1);
		}
		fs_path_put(cwd);
		if (!iabs) return cl + (size_t)slash + rl + 1 > PATH_MAX ? -36 : -12;
		ipath = iabs;
	}
	vfs_stat_t ist = {0};
	int ifd = (vfs_stat(ipath, &ist) != 0 || ist.size == 0) ? -1 : vfs_open(ipath, O_RDONLY, 0);
	if (iabs) kfree(iabs);
	if (ifd < 0) return -ENOEXEC;

	elf_info_t ii = {0};
	int irc = elf_load_fd(new_vm, ifd, (size_t)ist.size, en->mmap_base, &ii);
	vfs_close(ifd);
	if (irc != 0) return irc == -12 ? -12 : -8;

	en->jump_entry = ii.entry;
	en->at_base    = en->mmap_base;
	en->interp_lo  = ii.load_bias;
	en->interp_hi  = ii.load_end;
	/* keep subsequent mmap allocations above the interpreter image */
	en->mmap_base  = (ii.load_end + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
	return 0;
}

/* Map the user stack; remember the phys of the top ARG_WIN_PAGES pages so the
 * argv/envp/auxv block can be written through their kernel direct-map address
 * instead of raw user VAs. The caller destroys new_vm on failure. */
static long exec_map_stack(process_t *p, vm_space_t *new_vm, uint64_t argpg[ARG_WIN_PAGES])
{
	if (cgroup_charge_mem(new_vm, p->cgroup, (USER_STACK_PAGES_ + 1) * PAGE_SIZE) != 0)
		return -ENOMEM;
	for (int j = 0; j < USER_STACK_PAGES_; j++) {
		void *pg = buddy_alloc(0);
		if (!pg) return -ENOMEM;
		memset((void *)PHYS_TO_VIRT(pg), 0, PAGE_SIZE);
		uint64_t sva = USER_STACK_TOP_ - (uint64_t)(USER_STACK_PAGES_ - j) * PAGE_SIZE;
		vm_map(new_vm, sva, (uint64_t)pg, VM_USER | VM_READ | VM_WRITE);
		if (j >= USER_STACK_PAGES_ - ARG_WIN_PAGES)
			argpg[j - (USER_STACK_PAGES_ - ARG_WIN_PAGES)] = (uint64_t)pg;
	}
	void *guard = buddy_alloc(0);
	if (guard) {
		memset((void *)PHYS_TO_VIRT(guard), 0, PAGE_SIZE);
		uint64_t guard_va = USER_STACK_TOP_ - (uint64_t)(USER_STACK_PAGES_ + 1) * PAGE_SIZE;
		vm_map(new_vm, guard_va, (uint64_t)guard, VM_USER | VM_READ);
	}
	return 0;
}

/* comm, /proc/<pid>/exe and /proc/<pid>/cmdline for the new image */
static void exec_set_names(process_t *p, const char *kpath, const char *exe_real,
                           const exec_args_t *a)
{
	const char *last_slash = kpath;
	for (int j = 0; kpath[j]; j++) if (kpath[j] == '/') last_slash = kpath + j + 1;

	int nlen = 0;
	while (last_slash[nlen] && nlen < PROC_NAME_MAX - 1) {
		p->name[nlen] = last_slash[nlen];
		nlen++;
	}
	p->name[nlen] = '\0';

	/* remember the full path for /proc/<pid>/exe */
	const char *exe_src = exe_real[0] ? exe_real : kpath;
	int el = 0;
	while (exe_src[el] && el < PROC_NAME_MAX - 1) { p->exe_path[el] = exe_src[el]; el++; }
	p->exe_path[el] = '\0';

	/* publish the real argv for /proc/<pid>/cmdline (ps full command, pgrep -f) */
	proc_set_cmdline(p, a->kstrs, (uint32_t)a->argv_bytes);
}

/* close O_CLOEXEC file descriptors before exec */
static void exec_close_cloexec(process_t *p)
{
	if (!p->fds) return;
	for (int i = 0; i < p->fds->cap; i++) {
		if (!fd_is_cloexec(p->fds, i)) continue;
		vfs_file_t *xf = fd_remove(p->fds, i);   /* atomic detach + clears cloexec */
		if (xf) fput(xf);
		else    fd_set_cloexec(p->fds, i, 0);    /* stale bit, slot not open */
	}
}

/* reset signal state: SIG_IGN stays, user handlers reset to SIG_DFL. If the
 * disposition table is still shared with sibling threads, detach to a private
 * copy first so the reset doesn't reach them. */
static void exec_reset_signals(process_t *p)
{
	p->altstack = (stack_t){0};   /* the old image's stack is gone */
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
	sigqueue_drop(p);
	ptimer_drop(p->tgid);
}

/* apply credentials: suid/sgid set to new euid/egid after exec */
static void exec_apply_creds(process_t *p, uid_t new_euid, gid_t new_egid)
{
	p->euid = new_euid; p->suid = new_euid;
	p->egid = new_egid; p->sgid = new_egid;
	p->fs_uid_set = p->fs_gid_set = 0;   /* fsuid/fsgid = the new euid/egid */

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
}

/* push the auxv (16 bytes per pair, stack grows down) and keep a copy for
 * /proc/<pid>/auxv; returns the new sp */
static uint64_t exec_push_auxv(process_t *p, uint8_t *kbuf, uint64_t sp, const elf_info_t *ei,
                               const exec_entry_t *en, uint64_t at_execfn_addr,
                               uint64_t at_random_addr, int secure_exec)
{
#define PUSH_AUX(k, v) do { \
	sp -= 16; \
	((uint64_t *)KB(sp))[0] = (uint64_t)(k); \
	((uint64_t *)KB(sp))[1] = (uint64_t)(v); \
} while (0)

	uint64_t aux_hi = sp;
	PUSH_AUX(0,  0);
	/* AT_SYSINFO_EHDR advertisement gated off until the signal-delivery latency
	 * to syscall-free loops (exposed by c-itimer) is root-caused. The vDSO is
	 * built, mapped and kept fresh; flip this on once that's fixed. */
	if (vdso_advertise) PUSH_AUX(33, VDSO_VADDR);
	PUSH_AUX(31, at_execfn_addr);
	PUSH_AUX(25, at_random_addr);
	PUSH_AUX(23, secure_exec ? 1 : 0);   /* AT_SECURE */
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
	PUSH_AUX(9,  ei->entry);
	PUSH_AUX(7,  en->at_base);
	PUSH_AUX(6,  PAGE_SIZE);
	PUSH_AUX(5,  ei->phnum);
	PUSH_AUX(4,  ei->phentsize);
	PUSH_AUX(3,  ei->phdr_va);
#undef PUSH_AUX
	uint64_t an = aux_hi - sp;
	if (an > sizeof(p->saved_auxv)) an = sizeof(p->saved_auxv);
	memcpy(p->saved_auxv, KB(sp), (size_t)an);
	p->saved_auxv_len = (uint32_t)an;
	return sp;
}

/* Stage the SysV initial stack (argv/envp/auxv) in a kernel buffer, then copy
 * it into the new stack's top pages via their kernel direct-map address rather
 * than storing through raw user VAs. User-VA stores into the freshly-switched
 * address space, with IRQs enabled in the syscall, are the execve corruption:
 * a nested trap re-enters mid-build with the half-initialised new vm active. The
 * staged copy is address-space- and IRQ-independent. The destination pages are
 * the already-mapped top stack pages (argpg[], captured above). */
/* Build the SysV ABI initial stack (strings, AT_RANDOM, execfn, auxv, envp[],
 * argv[], argc) in kbuf, laid out for user addresses below USER_STACK_TOP_.
 * Frees the staged strings. Returns the initial sp. */
static uint64_t exec_fill_stack(process_t *p, uint8_t *kbuf, uint64_t *uargv, uint64_t *uenvp,
                                exec_args_t *a, const char *kpath, const elf_info_t *ei,
                                const exec_entry_t *en, int no_aslr, int secure_exec)
{
	int argc = a->argc, envc = a->envc;
	memset(uargv, 0, (size_t)(argc + 1) * sizeof(uint64_t));
	memset(uenvp, 0, (size_t)(envc + 1) * sizeof(uint64_t));

	/* ASLR: randomize stack top within last stack page (up to 4080 bytes) */
	uint64_t sp = USER_STACK_TOP_ - (no_aslr ? 0 : ((rng_next() & 0xffULL) << 4));
	for (int j = envc - 1; j >= 0; j--) {
		int n = 0; while (a->kenvp[j][n]) n++;
		sp -= (uint64_t)(n + 1);
		memcpy(KB(sp), a->kenvp[j], (size_t)(n + 1));
		uenvp[j] = sp;
	}
	for (int j = argc - 1; j >= 0; j--) {
		int n = 0; while (a->kargv[j][n]) n++;
		sp -= (uint64_t)(n + 1);
		memcpy(KB(sp), a->kargv[j], (size_t)(n + 1));
		uargv[j] = sp;
	}
	exec_args_free(a);
	sp &= ~15ULL;

	/* 16 random bytes for AT_RANDOM (libc stack canary / pointer guard): must be
	 * cryptographic, so draw from the CSPRNG rather than the ASLR PRNG. */
	sp -= 16;
	{
		csprng_bytes((void *)KB(sp), 16);
	}
	uint64_t at_random_addr = sp;

	/* execfn string */
	{
		int plen = 0; while (kpath[plen]) plen++;
		sp -= (uint64_t)(plen + 1);
		memcpy(KB(sp), kpath, (size_t)(plen + 1));
	}
	uint64_t at_execfn_addr = sp;

	sp &= ~15ULL;

	/* padding before auxv keeps the final sp 16-byte aligned;
	   must NOT be placed between envp[] and auxv — musl would read it as AT_NULL */
	if ((argc + envc + 3) % 2 != 0) {
		sp -= 8;
		*(uint64_t *)KB(sp) = 0;
	}
	sp = exec_push_auxv(p, kbuf, sp, ei, en, at_execfn_addr, at_random_addr, secure_exec);

	/* envp[] */
	sp -= 8; *(uint64_t *)KB(sp) = 0;
	for (int j = envc - 1; j >= 0; j--) { sp -= 8; *(uint64_t *)KB(sp) = uenvp[j]; }
	/* argv[] */
	sp -= 8; *(uint64_t *)KB(sp) = 0;
	for (int j = argc - 1; j >= 0; j--) { sp -= 8; *(uint64_t *)KB(sp) = uargv[j]; }
	/* argc */
	sp -= 8; *(uint64_t *)KB(sp) = (uint64_t)argc;
	return sp;
}

/* Enter the new image: the syscall frame gets the entry point and stack, and
 * the TLS registers start clear. */
static void exec_start_context(process_t *p, uint64_t jump_entry, uint64_t sp)
{
	/* p->ctx is whatever frame the last sleep or preemption saved; loading a
	 * file can do either, and the entry point must land in the syscall's frame */
	p->ctx = p->uctx;
	arch_context_init_user(p->ctx, jump_entry, sp);

	/* reset TLS */
	p->tls_base = 0;
#if defined(__x86_64__)
	p->user_gs_base = 0;
	__asm__ volatile("wrmsr" :: "c"(0xC0000102U), "a"(0U), "d"(0U));
#endif
#if defined(__aarch64__)
	__asm__ volatile("msr tpidr_el0, xzr");
#elif defined(__x86_64__)
	__asm__ volatile("wrmsr" :: "c"(0xC0000100U), "a"(0U), "d"(0U));
#endif
}

/* The point of no return: switch to the new address space and record its
 * initial VMAs. The old one's VMAs are freed by vm_destroy when its last
 * reference drops, and new_vm starts with an empty VMA list. */
static void exec_switch_vm(process_t *p, vm_space_t *new_vm, const elf_info_t *ei,
                           const exec_entry_t *en, int secure_exec)
{
	vm_space_t *old_vm = p->vm;
	p->vm        = new_vm;
	p->vm->brk   = ei->load_end;
	p->vm->mmap_next = en->mmap_base;
	p->dumpable  = secure_exec ? 0 : 1;
	ptrace_exec_done(p);
	vfork_release(p);
	arch_vm_switch((uint64_t)new_vm->root, new_vm->asid);
	if (old_vm) vm_destroy(old_vm);

	/* record initial VMAs */
	vma_add(p, USER_STACK_TOP_ - USER_STACK_PAGES_ * PAGE_SIZE,
	        USER_STACK_PAGES_ * PAGE_SIZE, VMA_USER | VMA_READ | VMA_WRITE);
	vma_add(p, ei->load_bias, ei->load_end - ei->load_bias,
	        VMA_USER | VMA_READ | VMA_WRITE | VMA_EXEC);
	if (en->interp_hi > en->interp_lo)
		vma_add(p, en->interp_lo, en->interp_hi - en->interp_lo,
		        VMA_USER | VMA_READ | VMA_WRITE | VMA_EXEC);
}

/* kpath: a kmalloc'd PATH_MAX kernel buffer, consumed. argv and envp are user. */
static long do_execve(char *kpath, char * const argv[], char * const envp[])
{
	process_t *p = task_current();
	if (exec_make_absolute(p, kpath)) { kfree(kpath); return -ENAMETOOLONG; }

	char *shebang_pre[SHEBANG_MAX_DEPTH * 3];
	int   shebang_n = 0;
	long r = exec_resolve_shebang(p, &kpath, shebang_pre, &shebang_n);
	if (r) return r;

	exec_args_t a;
	r = exec_stage_args(&a, shebang_pre, shebang_n, argv, envp);
	if (r) { kfree(kpath); return r; }

	/* free the heap staging that is no longer needed once a pre-switch step fails
	 * (the args survive until the user stack is built; kpath until the exe path
	 * is recorded). */
#define EXEC_STAGE_FREE() do { kfree(kpath); exec_args_free(&a); } while (0)

	/* read file from VFS */
	vfs_stat_t st = {0};
	if (vfs_stat(kpath, &st) != 0) { EXEC_STAGE_FREE(); return -ENOENT; }
	if (st.size == 0)              { EXEC_STAGE_FREE(); return -ENOEXEC; } /* ENOEXEC */

	unsigned long mnt_flags = vfs_path_mnt_flags(kpath);
	if (mnt_flags & MS_NOEXEC) { EXEC_STAGE_FREE(); return -EACCES; } /* EACCES */

	/* AT_SECURE/env-scrub kick in only on an id change */
	uid_t new_euid;
	gid_t new_egid;
	exec_new_creds(p, &st, mnt_flags, &new_euid, &new_egid);
	int secure_exec = (new_euid != p->euid) || (new_egid != p->egid);
	if (secure_exec) a.envc = exec_scrub_env(a.kenvp, a.envc);

	int fd = vfs_open(kpath, O_RDONLY, 0);
	if (fd < 0) { EXEC_STAGE_FREE(); return -ENOENT; }
	char exe_real[PROC_NAME_MAX];
	exec_real_path(p, fd, exe_real);

	/* build new address space */
	vm_space_t *new_vm = vm_create();
	if (!new_vm) { vfs_close(fd); EXEC_STAGE_FREE(); return -ENOMEM; }

	elf_info_t ei = {0};
	int load_rc = elf_load_fd(new_vm, fd, (size_t)st.size, 0, &ei);
	vfs_close(fd);
	if (load_rc != 0) {
		vm_destroy(new_vm);
		EXEC_STAGE_FREE();
		return load_rc == -12 ? -12 : -8;
	}

	/* entry point and aux values; overridden for dynamic executables */
	exec_entry_t en = { .jump_entry = ei.entry };
	/* ASLR mmap base; the dynamic linker (if any) loads at its bottom.
	 * personality ADDR_NO_RANDOMIZE (0x0040000) pins it (setarch -R, debuggers).
	 * 24 page-bits of entropy → base in [1 GB, ~65 GB), leaving the rest of the
	 * 256 GB span for the mmap window below the 1 GB-hugepage ceiling. */
	/* a personality set by the caller must not weaken a setuid image */
	int no_aslr = (task_current()->personality & 0x0040000) != 0 && !secure_exec;
	en.mmap_base = USER_MMAP_BASE_ + (no_aslr ? 0 : ((rng_next() & 0xffffffULL) << 12));
	/* the bump allocator never skips: an image with no interpreter sits at
	 * 4 GB, so the window starts past it */
	if (!ei.interp[0] && ei.load_bias) en.mmap_base += ei.load_bias;

	uint64_t argpg[ARG_WIN_PAGES];
	if ((ei.interp[0] && (r = exec_load_interp(p, new_vm, &ei, &en)) != 0) ||
	    (r = exec_map_stack(p, new_vm, argpg)) != 0) {
		vm_destroy(new_vm); EXEC_STAGE_FREE(); return r;
	}

	vdso_map(new_vm);   /* REALTIME/gettimeofday fast path (AT_SYSINFO_EHDR below) */

	exec_set_names(p, kpath, exe_real, &a);
	exec_close_cloexec(p);
	exec_reset_signals(p);
	exec_apply_creds(p, new_euid, new_egid);

	/* Reject before the point of no return (the VM switch) if the assembled
	 * initial stack — strings + execfn + AT_RANDOM + auxv + the argv/envp pointer
	 * vectors — would not fit the arg window. Beyond this the KB() mask would wrap
	 * and silently corrupt; -E2BIG matches Linux. ~512 bytes covers auxv+padding. */
	size_t need = (size_t)a.koff + PATH_MAX + 16 + 512 +
	              (size_t)(a.argc + a.envc + 3) * sizeof(uint64_t);
	if (need >= ARG_WIN_BYTES) { EXEC_STAGE_FREE(); return -E2BIG; }

	uint64_t *uargv = kmalloc((size_t)(a.argc + 1) * sizeof(uint64_t));
	uint64_t *uenvp = kmalloc((size_t)(a.envc + 1) * sizeof(uint64_t));
	uint8_t  *kbuf  = kmalloc(ARG_WIN_BYTES);
	if (!uargv || !uenvp || !kbuf) {
		kfree(uargv); kfree(uenvp); kfree(kbuf); EXEC_STAGE_FREE(); return -ENOMEM;
	}
	memset(kbuf, 0, ARG_WIN_BYTES);

	exec_switch_vm(p, new_vm, &ei, &en, secure_exec);

	uint64_t sp = exec_fill_stack(p, kbuf, uargv, uenvp, &a, kpath, &ei, &en,
	                              no_aslr, secure_exec);

	/* publish the staged stack into the new address space via the pages' kernel
	 * direct-map address — no user-VA stores, so a nested trap can't land them in
	 * the wrong page. Only the pages actually written ([sp, top)) need copying. */
	int first_pg = (int)((sp - ARG_BASE) / PAGE_SIZE);
	if (first_pg < 0) first_pg = 0;
	for (int _i = first_pg; _i < ARG_WIN_PAGES; _i++)
		memcpy((void *)PHYS_TO_VIRT(argpg[_i]),
		       kbuf + (uint64_t)_i * PAGE_SIZE, PAGE_SIZE);
	kfree(kbuf);
	kfree(uargv);
	kfree(uenvp);
	kfree(kpath);
#undef EXEC_STAGE_FREE

	exec_start_context(p, en.jump_entry, sp);
	return 0;
}
#undef KB
#undef ARG_BASE
#undef ARG_WIN_PAGES
#undef ARG_WIN_BYTES
#undef ARG_STR_MAX

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
	task_exit();
	return 0;
}

static long sys_exit_group(int code)
{
	task_group_exit(task_current(), code, 0);
	return sys_exit(code);
}

static long sys_getpid(void)
{
	process_t *p = task_current();
	pid_t glead = p->tgid ? p->tgid : p->pid;
	process_t *leader = (glead == (pid_t)p->pid) ? NULL : proc_get(glead);
	int nr = task_pid_nr_ns(leader ? leader : p, current_pid_ns());
	proc_put(leader);
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
	if (flags) return -EINVAL;
	if (lcnt > 16 || rcnt > 16) return -EINVAL;   /* (we cap at 16 iovecs) */
	uid_t teuid = 0;
	process_t *target = proc_get_user(pid);
	vm_space_t *tvm = target ? proc_get_vm_cred((pid_t)target->pid, &teuid) : NULL;   /* pinned; vm_destroy to release */
	proc_put(target);
	if (!tvm) return -ESRCH;
	process_t *cur = task_current();
	long ret;

	if (cur->euid != 0 && cur->euid != teuid) {
		ret = -1;                                      /* EPERM */
	} else {
		iovec_t liov[16], riov[16];
		if (copy_from_user(liov, uliov, lcnt * sizeof(iovec_t)) != 0 ||
		    copy_from_user(riov, uriov, rcnt * sizeof(iovec_t)) != 0) {
			ret = -14;
		} else {
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
				size_t got;
				if (write) {
					if (copy_from_user(tmp, (void *)lva, n) != 0) break;
					got = vm_access_remote(tvm, rva, tmp, n, 1);
				} else {
					got = vm_access_remote(tvm, rva, tmp, n, 0);
					if (got && copy_to_user((void *)lva, tmp, got) != 0) break;
				}
				lo += got; ro += got; total += (long)got;
				if (got < n) break;
			}
			ret = total ? total : ((lcnt && rcnt) ? -14 : 0);
		}
	}
	vm_destroy(tvm);
	return ret;
}

static long sys_vfork(uint64_t newsp, uint64_t flags)
{
	/* vfork-as-fork: give the child its own address space instead of
	 * sharing the parent's stack. Sharing is unsafe under SMP — the
	 * parent and child can run concurrently and corrupt the shared
	 * user stack. fork semantics are always a valid vfork. The child still
	 * runs on its own stack when the caller passes one (posix_spawn). */
	/* The parent still waits, as vfork promises: until the child execs or exits.
	 * Programs count on it, and a chain of vforks that did not wait multiplies. */
	process_t *p = task_current();
	p->vfork_pending = 1;
	/* Chromium shares its root; namespace flags make them as fork does */
	long pid = (long)proc_fork(flags & (CLONE_FS | CLONE_NEW_ANY), newsp);
	if (pid < 0) { p->vfork_pending = 0; return pid; }
	while (!task_has_actionable_signal()) {
		/* checked under the lock task_wake takes, so the release cannot slip
		 * between the test and the sleep; no deadline, no tick sweep */
		uint64_t f = spin_lock_irq(&sched_lock);
		if (!p->vfork_pending) { spin_unlock_irq(&sched_lock, f); break; }
		task_sleep_locked(f);
	}
	p->vfork_pending = 0;
	return pid;
}

static long sys_pipe2(int *pipefd, int flags)
{
	if (flags & ~(O_NONBLOCK | O_CLOEXEC)) return -EINVAL;
	int kfds[2];
	int pr = vfs_pipe(kfds);
	if (pr != 0) return pr == -24 ? -24 : -12;
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
	if (copy_to_user(pipefd, kfds, sizeof(int) * 2) != 0) {
		vfs_close(kfds[0]);
		vfs_close(kfds[1]);
		return -EFAULT;
	}
	return 0;
}



/* wait4 rusage: the reaped child's CPU time, including descendants it waited
 * for (Linux folds those into the child's own ru_*). Must run before proc_reap
 * frees the struct. */
static void fill_wait_rusage(void *urusage, process_t *child)
{
	if (!urusage) return;
	int64_t r[18];
	memset(r, 0, sizeof(r));
	uint64_t hz = TIMER_HZ;
	uint64_t ut = child->utime_ticks + child->cutime_ticks;
	uint64_t st = child->stime_ticks + child->cstime_ticks;
	r[0] = (int64_t)(ut / hz); r[1] = (int64_t)((ut % hz) * (1000000 / hz));
	r[2] = (int64_t)(st / hz); r[3] = (int64_t)((st % hz) * (1000000 / hz));
	copy_to_user(urusage, r, sizeof(r));
}

/* One wait: ws gets the status word, pid number and uid of the child it
 * reports (wait_snap_t). flags: WNOHANG, WUNTRACED, WCONTINUED, WEXITED (an
 * exit counts only with it) and WNOWAIT (report, reap nothing). */
static long do_waitpid(pid_t pid, wait_snap_t *ws, int flags, void *rusage)
{
	process_t *p = task_current();
	int wnohang  = (flags & WNOHANG) != 0;

	/* -1: any child; 0: any in my process group; < -1: any in group -pid */
	if (pid <= 0) {
		pid_t pgrp = pid == -1 ? -1 : pid == 0 ? p->pgrp : pid_from_user(-pid);
		if (pgrp == 0 && pid < -1) return -ECHILD;   /* no such group in my pid namespace */
		int intr = 0;
		while (1) {
			process_t *child = NULL;
			int any_alive = 0, event = 0;
			proc_find_zombie_or_sleep(p->tgid, p, pgrp, !wnohang && !intr, flags, &child, &any_alive, &event, ws);
			if (child && event == 1 && !(flags & WNOWAIT)) {
				fill_wait_rusage(rusage, child);
				if (child->pid == child->tgid && child->ppid != p->tgid) {
					/* a tracee that is someone else's child: they reap it */
					child->tracer = 0;
					child->reaped = 0;
					proc_wake_waiters(child->ppid);
					return (long)ws->nr;
				}
				proc_reap(child);
				return (long)ws->nr;
			}
			/* a stop, a continue, a ptrace stop, or an exit left waitable */
			if (child) return (long)ws->nr;
			if (!any_alive) return -ECHILD;
			if (wnohang)    return 0;
			if (intr && any_alive != 2) return -EINTR;   /* 2: one is mid-exit, wait for it */
			arch_yield();
			p->in_wait = 0;
			/* an exit and its SIGCHLD come together: look once more, and
			 * be interrupted only if there is still nothing to report */
			if (task_has_actionable_signal()) intr = 1;
		}
	}

	process_t *child = pid > 0 ? proc_find_child(p->tgid, current_pid_ns(), (int)pid) : NULL;
	if (!child) return -ECHILD;

	int intr = 0;
	while (1) {
		int r = proc_check_or_sleep(child, p, !wnohang && !intr, flags, ws);
		if (r < 0) return -ECHILD;   /* exited, and the caller did not ask for exits */
		if (r == 1) break;           /* zombie */
		if (r) return (long)pid;     /* ptrace stop, job stop, continued */
		if (wnohang) return 0;
		if (intr && child->state != PROC_DEAD) return -EINTR;   /* mid-exit: wait for it */
		arch_yield();
		p->in_wait = 0;
		if (task_has_actionable_signal()) intr = 1;   /* as above: look once more */
	}

	if (flags & WNOWAIT) return (long)pid;
	fill_wait_rusage(rusage, child);
	if (child->pid == child->tgid && child->ppid != p->tgid) {
		child->tracer = 0;
		proc_wake_waiters(child->ppid);
		return (long)pid;
	}
	child->reaped = 1;
	proc_reap(child);
	return (long)pid;
}

/* wait4: exits always count; WEXITED and WNOWAIT are waitid's (Linux EINVAL) */
#define WAIT4_KNOWN (WNOHANG | WUNTRACED | 8 /* WCONTINUED */ | 0x20000000 /* __WNOTHREAD */ | \
                     0x40000000 /* __WALL */ | 0x80000000u /* __WCLONE */)
static long sys_waitpid(pid_t pid, int *ustatus, int flags, void *rusage)
{
	if (flags & (WEXITED | WNOWAIT)) return -EINVAL;
	if ((unsigned)flags & ~(unsigned)WAIT4_KNOWN) return -EINVAL;
	if (pid == (pid_t)0x80000000) return -ESRCH;   /* INT_MIN: no group -INT_MIN */
	wait_snap_t ws = { 0 };
	long r = do_waitpid(pid, &ws, flags | WEXITED, rusage);
	if (r > 0 && ustatus && put_user(ws.status, ustatus) != 0) return -EFAULT;
	return r;
}

#if defined(__x86_64__)
#define ARCH_SET_GS 0x1001
#define ARCH_SET_FS 0x1002

static long sys_arch_prctl(int code, uint64_t addr)
{
	/* wrmsr #GPs in ring 0 on a non-canonical base */
	if ((code == ARCH_SET_FS || code == ARCH_SET_GS) && !access_ok((void *)addr, 0)) return -1;
	if (code == ARCH_SET_FS) {
		__asm__ volatile("wrmsr" :: "c"(0xC0000100U), "a"((uint32_t)addr), "d"((uint32_t)(addr >> 32)));
		task_current()->tls_base = addr;
		return 0;
	}
	if (code == ARCH_SET_GS) {
		/* When kernel is running, USER GS is stored in KERNEL_GS_BASE MSR. */
		__asm__ volatile("wrmsr" :: "c"(0xC0000102U), "a"((uint32_t)addr), "d"((uint32_t)(addr >> 32)));
		task_current()->user_gs_base = addr;
		return 0;
	}
	return -EINVAL;
}
#endif

