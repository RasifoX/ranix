/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
#ifndef PROC_H
#define PROC_H

#include <uapi/types.h>
#include <kernel/rlimit.h>
#include <uapi/limits.h>
#include <asm/context.h>
#include <kernel/mm/vm.h>
#include <kernel/vfs.h>
#include <kernel/config.h>
#include <kernel/pid_namespace.h>
#include <kernel/user_namespace.h>
#include <kernel/fs_struct.h>

#define PROC_NAME_MAX  256
#define PROC_MAGIC     0x52414e58u   /* "RANX" — set at PCB alloc, checked at teardown */

/* The PCB is buddy-allocated as a single power-of-two block of order 1 (2
 * pages). Every PCB alloc/free must pass PCB_ORDER (not a literal 0). */
#define PCB_ORDER      1

/* scheduling policies (sched_setscheduler) */
#define SCHED_NORMAL  0
#define SCHED_FIFO    1
#define SCHED_RR      2

#define NSIG           64
#define SIGRTMIN       32    /* libc keeps 32-34: musl sends SIGTIMER as a queued signal */
#define SIGRTMAX       64
#define SIGKILL        9
#define SIGSTOP        19    /* uncatchable + unblockable, like SIGKILL */
#ifndef SIGILL
#define SIGILL         4
#endif
#ifndef SIGTRAP
#define SIGTRAP        5
#endif
#ifndef SIGBUS
#define SIGBUS         7
#endif
#ifndef SIGFPE
#define SIGFPE         8
#endif
#define SIGSEGV        11
#define SIGTERM        15
#define SIGCHLD        17
#define SIGCONT        18
#define SIGTSTP        20
#define SIGTTIN        21
#define SIGTTOU        22
#define SIGHUP          1
#define SIGSYS         31    /* seccomp */

#define SI_USER        0     /* si_code: kill */
#define SI_QUEUE       (-1)  /* sigqueue */
#define SI_TKILL       (-6)  /* tgkill, tkill */

typedef struct {
	void (*sa_handler)(int);
	uint64_t sa_flags;
	void (*sa_restorer)(void);
	uint64_t sa_mask;
} sigaction_t;

/* Signal dispositions are a process-wide property in POSIX: every thread of a
 * thread group shares one table (Linux's sighand_struct, CLONE_SIGHAND). Heap-
 * allocated and refcounted so it outlives whichever thread created or last
 * touched it. fork() gets a private copy; pthread_create() shares. */
typedef struct sighand {
	sigaction_t actions[NSIG];
	int         refcount;
} sighand_t;

sighand_t *sighand_alloc(sighand_t *copy_from);   /* refcount 1; NULL src → zeroed */
sighand_t *sighand_get(sighand_t *sh);            /* refcount++ */
void       sighand_put(sighand_t *sh);            /* refcount--; frees at 0 */

/* Kernel-side siginfo for a queued RT signal. Mirrors the userspace siginfo_t
 * head (si_signo/errno/code) plus the sender's pid/uid and sigval payload at
 * the offsets musl reads (si_pid@16, si_uid@20, si_value@24). */
typedef struct sigqueue {
	struct sigqueue *next;
	int       si_signo;
	int       si_errno;
	int       si_code;
	pid_t     si_pid;
	uid_t     si_uid;
	uint64_t  si_value;   /* union sigval (sival_int / sival_ptr) */
} sigqueue_t;

#define SIGQUEUE_MAX  256   /* per-process cap; overflow → -EAGAIN */

/* Who sent a pending standard signal. Standard signals coalesce, so one record
 * per signal is enough: the first sender wins until the signal is taken (Linux
 * drops the siginfo of a second instance). Kernel-posted signals have none. */
typedef struct sig_from {
	int       si_code;
	pid_t     si_pid;
	uid_t     si_uid;
	uint64_t  si_value;
} sig_from_t;

typedef struct {
	void   *ss_sp;
	int     ss_flags;
	size_t  ss_size;
} stack_t;

#define SS_ONSTACK  1
#define SS_DISABLE  2

typedef enum {
	PROC_RUNNING  = 0,
	PROC_READY    = 1,
	PROC_DEAD     = 2,
	PROC_SLEEPING = 3,
	PROC_ZOMBIE   = 4,   /* exited, waiting for parent wait() */
} proc_state_t;

/*
 * Process Control Block.
 *
 * Field order is FIXED through `vm` — exceptions_asm.S encodes
 * offset 16 for `ctx` directly. Do not reorder the first 8 fields.
 */
typedef struct process {
	/* ── scheduler / asm-visible ─────────────── */
	uint64_t        pid;          /* offset 0  */
	uint64_t        state;        /* offset 8  */
	arch_context_t *ctx;          /* offset 16 — hardcoded in exceptions_asm.S */
	void          (*entry)(void); /* offset 24 */
	uint8_t        *kstack;       /* offset 32 */
	char            name[PROC_NAME_MAX]; /* offset 40 */
	struct process *next;         /* global process list */
	struct process *sched_next;    /* scheduler runqueue list */
	struct process *wait_next;    /* wait queue list */
	vm_space_t     *vm;

	/* ── scheduling ──────────────────────────── */
	int             ticks_remaining;
	uint64_t        ticks;          /* total CPU ticks (utime + stime) */
	uint64_t        utime_ticks;    /* ticks spent in user mode */
	uint64_t        stime_ticks;    /* ticks spent in kernel (syscalls) */
	uint64_t        cutime_ticks;   /* reaped children's user time (times(2)) */
	uint64_t        cstime_ticks;   /* reaped children's system time */
	int             nice;          /* -20..19, default 0 */
	int             oom_score_adj; /* -1000 (never) .. 1000, /proc/PID/oom_score_adj; inherited */
	int             uname_linux;   /* uname says "Linux": Linux-only programs (Java checks os.name); inherited, kept on exec */
	struct cpumask *cpu_affinity;  /* sched_setaffinity mask; NULL = all CPUs */
	int             sched_policy;  /* SCHED_NORMAL/FIFO/RR */
	int             rt_priority;   /* 1..99 for FIFO/RR, 0 for NORMAL */
	int             seccomp_mode;  /* 0 off, 1 strict, 2 filter */
	struct seccomp_filter *seccomp_filter;  /* newest filter of the chain; holds a reference */

	/* ── signals ─────────────────────────────── */
	uint64_t        sig_pending;
	uint64_t        sig_mask;             /* per-thread blocked mask */
	uint64_t        sig_saved_mask;       /* mask to return to, once sig_mask_saved */
	int             sig_mask_saved;       /* sigsuspend/pselect: sig_mask is temporary until the
	                                       * signal it let in is delivered or the syscall ends */
	struct sighand *sighand;              /* signal dispositions, shared across a thread group */
	struct sigqueue *sigq_head;           /* queued RT siginfo, FIFO per signo, lowest signo first */
	struct sigqueue *sigq_tail;
	int             sigq_count;
	uint32_t        sig_from_valid;       /* bit sig-1: sig_from[sig-1] names the sender */
	sig_from_t      sig_from[SIGRTMIN - 1];

	/* ── interval timers ───────────────────────── */
	uint64_t        itimer_value;     /* ITIMER_REAL: ticks until next SIGALRM (0 = off) */
	uint64_t        itimer_interval;  /* reload value (0 = one-shot) */
	uint64_t        vtimer_value;     /* ITIMER_VIRTUAL: user-CPU ticks until SIGVTALRM */
	uint64_t        vtimer_interval;
	uint64_t        ptimer_value;     /* ITIMER_PROF: user+sys-CPU ticks until SIGPROF */
	uint64_t        ptimer_interval;
	int             in_syscall;       /* 1 while executing a syscall (kernel CPU time) */
	uint8_t         sys_intr;             /* the syscall just ending returned EINTR, restartable */
	uint64_t        sys_arg0;             /* its first argument, which aarch64's return overwrites */
	int             fault_sig;            /* a synchronous fault's signal, until delivered */
	int             fault_code;           /* si_code */
	uint64_t        fault_addr, fault_err, fault_trap;
	uint64_t        fault_value;          /* si_value: SIGSYS's syscall | arch << 32 */
	int             fault_errno;          /* si_errno: SIGSYS's SECCOMP_RET_DATA */
	uint8_t         in_freeze;        /* stopped by cgroup.freeze on the way to user mode */
	uint8_t         in_wait;          /* asleep in wait: any thread of the group may be woken by a child */
	int             last_syscall;     /* arch syscall number last entered (watchdog/diag) */
	uint64_t        ss_exit_ns;       /* ranix.slowsys: last syscall exit, time in faults since, */
	uint64_t        ss_fault_ns;      /*   and their count: where user time between syscalls went */
	uint32_t        ss_faults;
	int             ss_prevnr;

	/* ── process metadata ────────────────────── */
	uint64_t        brk;
	/* RLIMIT_* soft/hard pairs (RLIM_NLIMITS=16). NOFILE is enforced via
	 * fds->rlim_nofile and AS via the mmap path; the rest are store-and-report. */
	uint64_t        rlim_cur[RLIM_NLIMITS];   /* the group's, see kernel/rlimit.h */
	uint64_t        rlim_max[RLIM_NLIMITS];
	uint8_t         rlim_set;      /* 0 until first getrlimit/setrlimit seeds defaults */
	int             mlock_future;  /* mlockall(MCL_FUTURE): lock+prefault later mmaps */
	pid_t           ppid;
	pid_t           tgid;          /* thread group id (== pid for main thread) */
	pid_t           pgrp;
	pid_t           sid;           /* session id */
	uid_t           uid;   /* ruid */
	uid_t           euid;  /* effective uid */
	uid_t           suid;  /* saved set-uid */
	gid_t           gid;   /* rgid */
	gid_t           egid;  /* effective gid */
	gid_t           sgid;  /* saved set-gid */
	gid_t           groups[32]; /* supplementary groups */
	int             ngroups;
	int             dumpable;  /* 0=not dumpable, 1=dumpable (default) */
	int             no_new_privs; /* prctl(PR_SET_NO_NEW_PRIVS): sticky, inherited */
	uint8_t         mdwe;              /* PR_SET_MDWE: refuse W+X mappings; inherited, irreversible */
	int             pdeathsig;    /* prctl(PR_SET_PDEATHSIG): signal on parent death */
	uid_t           fsuid;     /* filesystem uid; fs_uid_set==0 → follows euid */
	gid_t           fsgid;
	uint8_t         fs_uid_set;
	uint8_t         fs_gid_set;
	uint32_t        personality; /* execution-domain word (ADDR_NO_RANDOMIZE etc.) */

	/* POSIX capabilities (lower 40 bits, caps 0-39) */
	uint64_t        cap_permitted;
	uint64_t        cap_effective;
	uint64_t        cap_inheritable;
	int             exit_code;
	int             killed_by_sig; /* non-zero if exited due to signal */
	int             group_exiting; /* a sibling ended the group; exit status already set */

	/* ── open files ──────────────────────────── */
	fd_table_t     *fds;
	struct fs_struct *fs;   /* root, cwd, umask: shared under CLONE_FS (fs_struct.h) */

	/* controlling terminal: object + its fops, set on TIOCSCTTY, cleared on
	 * exit/hangup. /dev/tty routes a session leader's I/O here. */
	void           *ctty;
	vfs_fops_t     *ctty_fops;

	/* virtual memory regions live on the shared vm_space (p->vm->vmas), so all
	 * threads of a process see one VMA list — see vma_add/vma_clip. */

	/* ── vfork ───────────────────────────────── */
	int             reaped;        /* 1 if waitpid has already returned this child */

	/* ── arch TLS base (FS_BASE on x86_64, TPIDR_EL0 on AArch64) ── */
	uint64_t        tls_base;
	uint64_t        user_gs_base;   /* x86_64 arch_prctl(ARCH_SET_GS); Wine keeps its TEB here */

	/* ── wait queue tracking ──────────────────── */
	int             sleeping_on_queue; /* 1 when sleeping via sleep_on/sleep_on_locked */
	uint64_t        wake_deadline;     /* tick to wake at (0 = none); for timed sleeps */
	uint64_t        poll_seen, poll_seen_any;   /* event counts when this task last armed */
#define POLL_KEYS 16
	const void     *poll_keys[POLL_KEYS];       /* what its poll is watching */
	uint64_t        poll_kseen[POLL_KEYS];
	int             poll_nk;                    /* -1: everything */
	pid_t           vfork_waiter;               /* a vfork child: the parent waiting for its exec or exit */
	volatile int    vfork_pending;              /* a vfork parent: the child has not let go yet */
	int             oom_victim;                 /* killed for memory, not gone yet */
	int             on_cpu;            /* 1 when currently executing on a CPU, protects against wake-up race condition */
	int             in_rq;             /* 1 when queued in a CPU runqueue (guards double-enqueue) */

	/* ── signal alternate stack ───────────────── */
	stack_t         altstack;

	/* ── job control (terminal stop/cont) ─────── */
	int             job_stopped;          /* 1 while stopped by SIGSTOP/TSTP/TTIN/TTOU */
	int             job_stop_sig;         /* the stop signal (waitpid WIFSTOPPED status) */
	int             job_stop_reported;    /* 1 = waitpid WUNTRACED already reported this stop */
	int             job_continued;        /* 1 = SIGCONT pending report (waitpid WCONTINUED) */

	/* ── ptrace ──────────────────────────────── */
	pid_t           tracer;
	int             ptrace_opts;          /* 0=none, 1=trace syscalls */
	int             ptrace_stop;          /* 0=running, 1=entry stop, 2=exit stop */
	int             ptrace_stop_reported; /* 1 = waitpid already returned this stop */
	int             ptrace_in_syscall;    /* currently sleeping in ptrace_do_stop */
	int             ptrace_options;       /* PTRACE_SETOPTIONS bits */
	int             ptrace_inject;        /* signal to deliver after a signal-delivery-stop */
	int             ptrace_event;         /* PTRACE_EVENT_ to report at the end of this syscall */
	uint64_t        ptrace_msg;           /* PTRACE_GETEVENTMSG */
	sigqueue_t      ptrace_si;            /* PTRACE_GETSIGINFO */
	int             ptrace_stop_sig;      /* WSTOPSIG for the current ptrace-stop (0 → SIGTRAP) */
	int             single_step;          /* PTRACE_SINGLESTEP: trap after each insn (TF on user return) */
	void           *uctx;                 /* last trap frame entered from ring3 (ptrace task_pt_regs) */
	uint32_t       *clear_child_tid;
	void           *robust_list;          /* set_robust_list head (user ptr); walked on exit for OWNER_DIED handoff */

	/* full path of the running image, for /proc/<pid>/exe */
	char            exe_path[PROC_NAME_MAX];

	/* NUL-separated argv captured at execve, for /proc/<pid>/cmdline.
	 * Bounded copy (Linux caps cmdline at the arg page); NULL until first exec. */
	char           *cmdline;
	uint32_t        cmdline_len;

	/* kernel_ticks when the PCB was created, for /proc/<pid>/stat starttime */
	uint64_t        start_tick;

	/* namespaces (shared on fork, copied on unshare) */
	struct uts_namespace  *uts_ns;
	struct ipc_namespace  *ipc_ns;
	struct time_namespace *time_ns;
	struct mnt_namespace  *mnt_ns;
	struct time_namespace *time_ns_new; /* pending time ns for children (unshare CLONE_NEWTIME) */

	/* pid namespace: ns this process lives in, a pending ns for its children
	 * (unshare CLONE_NEWPID), and its nr in each ancestor ns */
	struct pid_namespace *pid_ns;
	struct pid_namespace *pid_ns_new;
	struct { struct pid_namespace *ns; int nr; } pidnrs[PIDNS_MAX_LEVEL];
	int                   pid_levels;
	struct user_namespace *user_ns;
	vfs_dentry_t    resolved;      /* volatile result of the last path resolve */
	int             exiting;       /* in task_exit: a pending SIGKILL has done its job */
	struct net_namespace  *net_ns;

	/* cgroup membership (NULL = root cgroup) */
	void           *cgroup;
	struct cgroup_namespace *cgroup_ns;

	struct asess   *asess;         /* agent session; set before the task can run, cleared only at exit */
	int             walk_denied;   /* this syscall's path walk stopped at a directory it may not search */
	int             walking;       /* this syscall has walked a path; see the deferred frees in vfs.c */
	char           *walk_anchor[2]; /* this syscall's dirfd paths; see walk_anchored in vfs.c */
	struct vfs_dentry *walk_top;   /* this syscall's held directory above the root; see vfs_walk_from */
	uint8_t         walk_kinds;    /* this syscall's path starts: 1 the root, 2 walk_top */
	uint32_t        walk_gen;

	uint32_t        magic;         /* PROC_MAGIC; corruption/use-after-free tripwire */
	uint32_t        refcount;      /* outstanding proc_get pins; PCB freed once 0 and pcb_dead */
	uint8_t         pcb_dead;      /* proc_reap tore the PCB down; the last proc_put frees it */

	/* ── hung-task detector (khungtaskd) ──────────────────────── */
	uint8_t         hung_watch;        /* 1 = an uninterruptible block the detector must time out */
	uint64_t        hung_switches;     /* dispatch count: short-lived wakeups progress without being on-CPU at a tick */
	uint64_t        hung_run_ticks;    /* ticks+dispatches at the last progress check (frozen ⇒ no progress) */
	uint64_t        hung_stall_start;  /* kernel_ticks when the current no-progress window opened (0 = none) */
	/* kill-always-wins watch: kernel_ticks when SIGKILL was first posted (0 = never),
	 * and when the last "ignored SIGKILL" report went out */
	uint64_t        kill_posted_at;
	uint64_t        kill_reported_at;

	/* the auxiliary vector exec gave the image, for /proc/<pid>/auxv: gdb finds
	 * a PIE's load address (AT_PHDR, AT_ENTRY) and ld.so (AT_BASE) there */
	uint64_t        saved_auxv[48];
	uint32_t        saved_auxv_len;    /* bytes, AT_NULL pair included */
} process_t;

/* scheduler */
/* scheduler */
void       sched_init(void);
void       sched_tick(void);
void       schedule(void);
void       sched_free_dying_kstack(void);
void       sched_requeue_prev(void);
void       yield(void);
void       task_sleep(void);
void       task_sleep_until(uint64_t deadline);
void       task_sleep_interruptible(void);
/* Sleep once if cond still holds, tested under sched_lock with interrupts off.
 * Whoever makes it false and then wakes the task cannot fall between the test and
 * the sleep, as with an unlocked check before task_sleep_interruptible(). cond must
 * not take sched_lock. Use it in a loop. */
extern spinlock_t sched_lock;
void task_sleep_locked(uint64_t irq_flags);
#define task_sleep_if(cond) do { \
	uint64_t f_ = spin_lock_irq(&sched_lock); \
	if (cond) task_sleep_locked(f_); \
	else spin_unlock_irq(&sched_lock, f_); \
} while (0)
/* Lower the sched_tick deadline-sweep gate to `deadline` (ticks). Callers that
 * arm p->wake_deadline must call this so the sweep doesn't sleep past it. */
void       sched_note_wake_deadline(uint64_t deadline);
/* Adjust the count of armed ITIMER_REAL timers when itimer_value transitions
 * between 0 and non-0. Caller holds sched_lock. */
void       sched_itimer_account(uint64_t old_val, uint64_t new_val);
void       task_sleep_unlock(spinlock_t *lock, uint64_t lock_flags);
void       task_wake(process_t *p);
void       task_wake_no_resched(process_t *p);
int        task_has_actionable_signal(void);
/* Kill-always-wins (docs: arastirma/ajan-insan-ozellikleri.md). fatal_signal_pending:
 * p must die (SIGKILL posted, its group is exiting). kloop_should_stop: for long or
 * data-driven kernel loops; yields when the tick asks and returns 1 when the caller
 * must abandon the loop (return -EINTR). Call it at least once per iteration of any
 * loop whose bound comes from user data. */
int        fatal_signal_pending(process_t *p);
int        kloop_should_stop(void);
void       task_signal(process_t *p, int sig);
void       task_signal_group(process_t *leader, int sig);
void       task_group_exit(process_t *p, int code, int by_sig);
void       task_signal_pgrp(pid_t pgrp, int sig);
int        task_pgrp_in_session(pid_t pgrp, pid_t sid);
int        signal_may_send(process_t *sender, process_t *t, int sig);
int        task_in_group(process_t *p, gid_t g);   /* egid or a supplementary group */
int        task_kill_many(process_t *sender, pid_t pgrp, int sig);
void       task_kill_session(struct asess *s);
/* The calling task as a signal to t names it: its thread group id in t's pid
 * namespace (0 if not visible there) and its real uid in t's user namespace. */
void       signal_sender(process_t *t, pid_t *pid, uid_t *uid);
/* kill, tgkill, sigqueue: post sig with its siginfo. An RT signal queues it, a
 * standard one keeps it unless the signal is already pending.
 * thread_directed != 0 targets the exact thread `p`; otherwise it is process-
 * directed (any eligible thread of p's group). Returns 0 or -EAGAIN on overflow. */
int        task_signal_info(process_t *p, int sig, int si_code, pid_t si_pid,
                            uid_t si_uid, uint64_t si_value, int thread_directed);
void       sigqueue_drop(process_t *p);   /* free all queued siginfo (exec/exit) */
/* A standard signal's pending bit was cleared without taking it: drop its sender
 * too, or the next kernel-posted instance would report it. Under sched_lock. */
static inline void sig_from_forget(process_t *t, uint64_t bits)
{
	t->sig_from_valid &= (uint32_t)~bits;
}
/* Job-control background-I/O check for a controlling tty. `tty`/`tty_fops`
 * identify the terminal; `pgrp` is its foreground group. Returns 0 to proceed,
 * 1 to retry (the caller stopped and was continued), or -EIO. `sig` is SIGTTIN
 * (read) or SIGTTOU (write). */
int        tty_jobctl_check(void *tty, vfs_fops_t *tty_fops, pid_t fg_pgrp, int sig);
/* SIGHUP+SIGCONT a foreground group and detach the ctty of any process whose
 * controlling terminal is `tty`. */
void       tty_hangup(void *tty, pid_t fg_pgrp);
int        tty_acquire(void *tty, vfs_fops_t *fops, int steal);  /* 0 taken, 1 already held, -1 EPERM */
long       tty_check_fg(void *tty, int need_ctty, pid_t nr, pid_t *out);
void       signal_dispatch(arch_context_t *tf);
void       task_force_fault(arch_context_t *tf, int sig, int code, uint64_t addr, uint64_t err, uint64_t trap);
void       task_force_seccomp(uint64_t call_addr, int nr, uint32_t arch, int data);
int        task_fault_fatal(int sig);
int        task_dequeue_signal(process_t *p, uint64_t set, sigqueue_t *si, int *have);
void       task_exit(void);

process_t *task_current(void);

int        task_count(void);
void       copy_creds(process_t *child, const process_t *parent);   /* ids, groups, caps, limits */
int        proc_count_uid(uid_t uid);      /* live processes owned by ruid (RLIMIT_NPROC) */
process_t *task_get(int index);

/* process lifecycle */
pid_t      proc_create(const char *name, void (*fn)(void));
pid_t      proc_exec(const char *name, const void *elf_data, size_t elf_len);
void       proc_set_cmdline(process_t *p, const char *block, uint32_t len); /* replace /proc/<pid>/cmdline argv (NUL-separated) */
void       vma_add(process_t *p, uint64_t vaddr, uint64_t size, uint32_t flags);
void       vma_clip(process_t *p, uint64_t va, uint64_t len);
pid_t      proc_fork(uint64_t clone_flags, uint64_t newsp);
pid_t      proc_fork_asess(struct asess *s);   /* fork with the child inside s; see asess_spawn */
int        fs_shared_outside_group(process_t *p);   /* setuid exec must not raise ids */
pid_t      proc_clone_thread(uint64_t newsp, uint64_t tls, uint32_t *ctid, uint32_t *ptid, uint32_t *set_tid);
pid_t      proc_clone_thread_ex(uint64_t newsp, uint64_t tls, uint32_t *ctid, uint32_t *ptid, uint32_t *set_tid,
                                int as_process, int share_fs);
int        proc_move_cgroup(process_t *t, void *cg);  /* 0, -1 at pids.max, -3 no live thread */
process_t *proc_find(pid_t pid);           /* lookup, no reference — safe only under sched_lock or for an existence check */
process_t *proc_find_child(pid_t ppid, struct pid_namespace *ns, int nr);
process_t *proc_get(pid_t pid);            /* lookup + pin (get_task_struct); release with proc_put */
void       proc_hold(process_t *p);        /* pin a process you already hold a live reference to */
void       proc_put(process_t *p);         /* drop a proc_get/proc_hold pin (NULL-safe) */
process_t *proc_get_user(pid_t nr);        /* proc_get for a pid as the caller's pid namespace numbers it */
pid_t      pid_from_user(pid_t nr);        /* global pid of a caller-visible number; 0 if it has none */
pid_t      pid_to_user(pid_t global);      /* the caller's number for a global pid; 0 if it has none */
void       seccomp_fork(process_t *child, process_t *parent);
void       seccomp_put(struct seccomp_filter *f);
vm_space_t *proc_get_vm(pid_t pid);        /* pin a target's address space (/proc/<pid>/mem); release with vm_destroy */
vm_space_t *proc_get_vm_cred(pid_t pid, uid_t *out_euid);  /* + sample euid (process_vm_readv access check) */
fd_table_t *proc_get_fds(pid_t pid);       /* pin a target's fd table (/proc/<pid>/fd); release with fd_table_put */
int        ptrace_may_access(process_t *t);/* Linux ptrace_may_access: euid==all-target-ids+dumpable, or CAP_SYS_PTRACE */
int        proc_may_access(pid_t pid);     /* ptrace_may_access by pid (pins the target) — cross-process /proc gate */
void       cred_setxuid_fixup(process_t *p, int was_priv, uid_t old_euid); /* drop caps when last uid 0 leaves the {r,e,s}uid set */

/* namespace kinds for proc_get_ns (matches procfs ns-link ordering) */
enum { PROC_NS_UTS = 0, PROC_NS_IPC, PROC_NS_TIME, PROC_NS_MNT, PROC_NS_PID,
       PROC_NS_USER, PROC_NS_NET, PROC_NS_CGROUP };
/* pin a target's namespace of the given kind for an ns-link open; NULL if the
 * target is gone or the ns is. Release with the matching xxx_ns_put. */
void      *proc_get_ns(pid_t pid, int kind);
int        proc_thread_count(pid_t tgid);
/* fn on each other live thread of p's group, sched_lock held by the caller;
 * stops at and returns the first thread fn answers nonzero for, else NULL */
process_t *proc_other_threads_locked(process_t *p, int (*fn)(process_t *t, void *arg), void *arg);
void       proc_wait(pid_t pid);
int        proc_root_under(const void *top);
void       proc_reap(process_t *child);
void       proc_wake_waiters(pid_t tgid);
void       proc_wake_frozen(void);
void       proc_wake_tracer(process_t *p);
void       proc_find_zombie(pid_t ppid, process_t **out_child, int *out_any_alive);

/* set by sys_vfork so the asm syscall path reschedules after handler */
extern volatile int need_resched;

typedef process_t task_t;
#define task_create(name, fn)  proc_create(name, fn)
#define TASK_RUNNING  PROC_RUNNING
#define TASK_READY    PROC_READY
#define TASK_DEAD     PROC_DEAD
#define TASK_SLEEPING PROC_SLEEPING

/* scheduler internals other subsystems use (sched.c) */
/* proctab[i] with no internal lock: the caller already holds sched_lock
 * (task_get() locks internally and would self-deadlock under a sweep). */
process_t *task_peek_locked(int index);
int  sched_running_now(void);
extern uint64_t load_avg[3];
void kstack_check(void);
void cond_resched(void);
void sched_bsp_wake(void);                       /* from the BSP's idle loop */

/* proc.c */
int  oom_victim_pending(void);
void vfork_release(process_t *p);
void proc_find_zombie_or_sleep(pid_t ppid, process_t *parent, int will_sleep, int flags,
                               process_t **out_child, int *out_any_alive, int *out_event);
int  proc_check_or_sleep(process_t *child, process_t *parent, int will_sleep, int flags);

/* futexes (sys/syscall_proc.c) and POSIX timers (sys/syscall_timer.c) */
void futex_wake_addr(uint32_t *uaddr);
void futex_exit_robust(process_t *p);
void ptimer_drop(pid_t tgid);

/* coredump.c */
extern char core_pattern[128];
void coredump(process_t *p, int sig, arch_context_t *ctx);

#endif
