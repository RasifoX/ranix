#ifndef PROC_H
#define PROC_H

#include <uapi/types.h>
#include <uapi/limits.h>
#include <asm/context.h>
#include <kernel/mm/vm.h>
#include <kernel/vfs.h>
#include <kernel/config.h>
#include <kernel/pid_namespace.h>
#include <kernel/user_namespace.h>

#define PROC_NAME_MAX  256
#define PROC_MAGIC     0x52414e58u   /* "RANX" — set at PCB alloc, checked at teardown */

/* The PCB is buddy-allocated as a single power-of-two block. cwd holds a full
 * PATH_MAX path, so the struct no longer fits one page — use order 1 (2 pages).
 * Every PCB alloc/free must pass PCB_ORDER (not a literal 0). */
#define PCB_ORDER      1

/* scheduling policies (sched_setscheduler) */
#define SCHED_NORMAL  0
#define SCHED_FIFO    1
#define SCHED_RR      2

#define NSIG           64
#define SIGRTMIN       34
#define SIGRTMAX       64
#define SIGKILL        9
#define SIGSTOP        19    /* uncatchable + unblockable, like SIGKILL */
#define SIGSEGV        11
#define SIGTERM        15
#define SIGCHLD        17
#define SIGCONT        18
#define SIGTSTP        20
#define SIGTTIN        21
#define SIGTTOU        22
#define SIGHUP          1

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
	struct cpumask *cpu_affinity;  /* sched_setaffinity mask; NULL = all CPUs */
	int             sched_policy;  /* SCHED_NORMAL/FIFO/RR */
	int             rt_priority;   /* 1..99 for FIFO/RR, 0 for NORMAL */
	int             seccomp_mode;  /* 0 off, 1 strict, 2 filter */
	void           *seccomp_filter;  /* SECCOMP_MODE_FILTER program (sock_filter[]) */
	unsigned        seccomp_flen;    /* filter instruction count */

	/* ── signals ─────────────────────────────── */
	uint64_t        sig_pending;
	uint64_t        sig_mask;             /* per-thread blocked mask */
	struct sighand *sighand;              /* signal dispositions, shared across a thread group */
	struct sigqueue *sigq_head;           /* queued RT siginfo, FIFO per signo, lowest signo first */
	struct sigqueue *sigq_tail;
	int             sigq_count;

	/* ── interval timers ───────────────────────── */
	uint64_t        itimer_value;     /* ITIMER_REAL: ticks until next SIGALRM (0 = off) */
	uint64_t        itimer_interval;  /* reload value (0 = one-shot) */
	uint64_t        vtimer_value;     /* ITIMER_VIRTUAL: user-CPU ticks until SIGVTALRM */
	uint64_t        vtimer_interval;
	uint64_t        ptimer_value;     /* ITIMER_PROF: user+sys-CPU ticks until SIGPROF */
	uint64_t        ptimer_interval;
	int             in_syscall;       /* 1 while executing a syscall (kernel CPU time) */
	int             last_syscall;     /* arch syscall number last entered (watchdog/diag) */

	/* ── process metadata ────────────────────── */
	uint64_t        brk;
	/* RLIMIT_* soft/hard pairs (RLIM_NLIMITS=16). NOFILE is enforced via
	 * fds->rlim_nofile and AS via the mmap path; the rest are store-and-report. */
	uint64_t        rlim_cur[16];
	uint64_t        rlim_max[16];
	uint8_t         rlim_set;      /* 0 until first getrlimit/setrlimit seeds defaults */
	uint64_t        mmap_next;
	uint64_t        huge1g_next;   /* next VA for 1 GB hugepage mmaps (own window) */
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

	/* ── open files ──────────────────────────── */
	fd_table_t     *fds;
	char            cwd[PATH_MAX];
	mode_t          umask;
	void           *root;   /* chroot dir (vfs_dentry_t*); NULL = global root "/" */

	/* controlling terminal: object + its fops, set on TIOCSCTTY, cleared on
	 * exit/hangup. /dev/tty routes a session leader's I/O here. */
	void           *ctty;
	vfs_fops_t     *ctty_fops;

	/* ── virtual memory regions ──────────────── */
	vma_t          *vmas;

	/* ── vfork ───────────────────────────────── */
	pid_t           vfork_parent;  /* -1 if not a vfork child */
	int             reaped;        /* 1 if waitpid has already returned this child */

	/* ── arch TLS base (FS_BASE on x86_64, TPIDR_EL0 on AArch64) ── */
	uint64_t        tls_base;

	/* ── wait queue tracking ──────────────────── */
	int             sleeping_on_queue; /* 1 when sleeping via sleep_on/sleep_on_locked */
	uint64_t        wake_deadline;     /* tick to wake at (0 = none); for timed sleeps */
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
	int             single_step;          /* PTRACE_SINGLESTEP: trap after each insn (TF on user return) */
	void           *uctx;                 /* last trap frame entered from ring3 (ptrace task_pt_regs) */
	uint32_t       *clear_child_tid;
	void           *robust_list;          /* set_robust_list head (user ptr); walked on exit for OWNER_DIED handoff */

	/* full path of the running image, for /proc/<pid>/exe */
	char            exe_path[PROC_NAME_MAX];

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
	struct net_namespace  *net_ns;

	/* cgroup membership (NULL = root cgroup) */
	void           *cgroup;
	struct cgroup_namespace *cgroup_ns;
	long            mem_charged;   /* bytes charged to the cgroup memory controller */

	uint32_t        magic;         /* PROC_MAGIC; corruption/use-after-free tripwire */
	uint32_t        refcount;      /* outstanding proc_get pins; PCB freed once 0 and pcb_dead */
	uint8_t         pcb_dead;      /* proc_reap tore the PCB down; the last proc_put frees it */

	/* ── hung-task detector (khungtaskd) ──────────────────────── */
	uint8_t         hung_watch;        /* 1 = an uninterruptible block the detector must time out */
	uint64_t        hung_run_ticks;    /* p->ticks at the last progress check (frozen ⇒ no progress) */
	uint64_t        hung_stall_start;  /* kernel_ticks when the current no-progress window opened (0 = none) */
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
void       task_signal(process_t *p, int sig);
void       task_signal_group(process_t *leader, int sig);
void       task_signal_pgrp(pid_t pgrp, int sig);
int        task_pgrp_exists(pid_t pgrp);
int        task_signal_broadcast(process_t *except, int sig);
/* sigqueue(2)/rt_sigqueueinfo: deliver an RT signal carrying si_value.
 * thread_directed != 0 targets the exact thread `p`; otherwise it is process-
 * directed (any eligible thread of p's group). Returns 0 or -EAGAIN on overflow. */
int        task_signal_info(process_t *p, int sig, int si_code, pid_t si_pid,
                            uid_t si_uid, uint64_t si_value, int thread_directed);
void       sigqueue_drop(process_t *p);   /* free all queued siginfo (exec/exit) */
/* Job-control background-I/O check for a controlling tty. `tty`/`tty_fops`
 * identify the terminal; `pgrp` is its foreground group. Returns 0 to proceed,
 * 1 to retry (the caller stopped and was continued), or -EIO. `sig` is SIGTTIN
 * (read) or SIGTTOU (write). */
int        tty_jobctl_check(void *tty, vfs_fops_t *tty_fops, pid_t fg_pgrp, int sig);
/* SIGHUP+SIGCONT a foreground group and detach the ctty of any process whose
 * controlling terminal is `tty`. */
void       tty_hangup(void *tty, pid_t fg_pgrp);
void       signal_dispatch(arch_context_t *tf);
void       task_exit(void);

process_t *task_current(void);

int        task_count(void);
process_t *task_get(int index);

/* process lifecycle */
pid_t      proc_create(const char *name, void (*fn)(void));
pid_t      proc_exec(const char *name, const void *elf_data, size_t elf_len);
void       vma_add(process_t *p, uint64_t vaddr, uint64_t size, uint32_t flags);
void       vma_clip(process_t *p, uint64_t va, uint64_t len);
pid_t      proc_fork(uint64_t clone_flags, uint64_t newsp);
pid_t      proc_vfork(void);
void       proc_vfork_exec_notify(void);
pid_t      proc_clone_thread(uint64_t newsp, uint64_t tls, uint32_t *ctid, uint32_t *ptid, uint32_t *set_tid);
process_t *proc_find(pid_t pid);           /* lookup, no reference — safe only under sched_lock or for an existence check */
process_t *proc_get(pid_t pid);            /* lookup + pin (get_task_struct); release with proc_put */
void       proc_hold(process_t *p);        /* pin a process you already hold a live reference to */
void       proc_put(process_t *p);         /* drop a proc_get/proc_hold pin (NULL-safe) */
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
void       proc_wait(pid_t pid);
void       proc_reap(process_t *child);
void       proc_find_zombie(pid_t ppid, process_t **out_child, int *out_any_alive);

/* set by sys_vfork so the asm syscall path reschedules after handler */
extern volatile int need_resched;

typedef process_t task_t;
#define task_create(name, fn)  proc_create(name, fn)
#define TASK_RUNNING  PROC_RUNNING
#define TASK_READY    PROC_READY
#define TASK_DEAD     PROC_DEAD
#define TASK_SLEEPING PROC_SLEEPING

#endif
