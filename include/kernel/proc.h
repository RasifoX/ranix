#ifndef PROC_H
#define PROC_H

#include <uapi/types.h>
#include <asm/context.h>
#include <kernel/mm/vm.h>
#include <kernel/vfs.h>
#include <kernel/config.h>

#define PROC_NAME_MAX  256

#define NSIG           64
#define SIGKILL        9
#define SIGSEGV        11
#define SIGTERM        15
#define SIGCHLD        17

typedef struct {
	void (*sa_handler)(int);
	uint64_t sa_flags;
	void (*sa_restorer)(void);
	uint64_t sa_mask;
} sigaction_t;

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
	uint64_t        ticks;
	int             nice;          /* -20..19, default 0 */

	/* ── signals ─────────────────────────────── */
	uint64_t        sig_pending;
	uint64_t        sig_mask;
	sigaction_t     sig_actions[NSIG];

	/* ── interval timer (ITIMER_REAL → SIGALRM) ─ */
	uint64_t        itimer_value;     /* ticks until next SIGALRM (0 = disabled) */
	uint64_t        itimer_interval;  /* reload value (0 = one-shot) */

	/* ── process metadata ────────────────────── */
	uint64_t        brk;
	uint64_t        mmap_next;
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

	/* POSIX capabilities (lower 40 bits, caps 0-39) */
	uint64_t        cap_permitted;
	uint64_t        cap_effective;
	uint64_t        cap_inheritable;
	int             exit_code;
	int             killed_by_sig; /* non-zero if exited due to signal */

	/* ── open files ──────────────────────────── */
	fd_table_t     *fds;
	char            cwd[256];
	mode_t          umask;

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

	/* ── ptrace ──────────────────────────────── */
	pid_t           tracer;
	int             ptrace_opts;          /* 0=none, 1=trace syscalls */
	int             ptrace_stop;          /* 0=running, 1=entry stop, 2=exit stop */
	int             ptrace_stop_reported; /* 1 = waitpid already returned this stop */
	int             ptrace_in_syscall;    /* currently sleeping in ptrace_do_stop */
	uint32_t       *clear_child_tid;

	/* full path of the running image, for /proc/<pid>/exe */
	char            exe_path[PROC_NAME_MAX];

	/* namespaces (shared on fork, copied on unshare) */
	struct uts_namespace  *uts_ns;
	struct ipc_namespace  *ipc_ns;
	struct time_namespace *time_ns;
	struct mnt_namespace  *mnt_ns;
	struct time_namespace *time_ns_new; /* pending time ns for children (unshare CLONE_NEWTIME) */

	/* cgroup membership (NULL = root cgroup) */
	void           *cgroup;
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
void       task_sleep_unlock(spinlock_t *lock, uint64_t lock_flags);
void       task_wake(process_t *p);
void       task_wake_no_resched(process_t *p);
int        task_has_actionable_signal(void);
void       task_signal(process_t *p, int sig);
void       task_signal_pgrp(pid_t pgrp, int sig);
void       signal_dispatch(arch_context_t *tf);
void       task_exit(void);

process_t *task_current(void);

int        task_count(void);
process_t *task_get(int index);

/* process lifecycle */
pid_t      proc_create(const char *name, void (*fn)(void));
pid_t      proc_exec(const char *name, const void *elf_data, size_t elf_len);
pid_t      proc_fork(uint64_t clone_flags);
pid_t      proc_vfork(void);
void       proc_vfork_exec_notify(void);
pid_t      proc_clone_thread(uint64_t newsp, uint64_t tls, uint32_t *ctid, uint32_t *ptid, uint32_t *set_tid);
process_t *proc_find(pid_t pid);
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
