#include <kernel/syscall.h>
#include <uapi/vdso.h>
#include <kernel/capabilities.h>
#include <kernel/proc.h>
#include <kernel/nsproxy.h>
#include <kernel/cgroup.h>
#include <kernel/elf.h>
#include <kernel/vfs.h>
#include <kernel/cmdline.h>
#include <kernel/hal.h>
#include <kernel/kprintf.h>
#include <kernel/mm/buddy.h>
#include <kernel/mm/pmm.h>
#include <kernel/mm/heap.h>
#include <kernel/mm/mmu.h>
#include <kernel/mm/vm.h>
#include <kernel/mm/hugetlb.h>
#include <kernel/mm/oom.h>
#include <kernel/lib/string.h>
#include <kernel/sync.h>
#include <kernel/smp.h>
#include <kernel/tty/fb_tty.h>
#include <drivers/serial/uart.h>
#include <kernel/board.h>
#include <kernel/net/socket.h>
#include <kernel/net/udp.h>
#include <kernel/net/icmp.h>
#include <kernel/net/ip.h>
#include <kernel/net/ipv6.h>
#include <kernel/net/icmpv6.h>
#include <kernel/net/tcp.h>
#include <kernel/net/net.h>
#include <kernel/net/netfilter.h>
#include <kernel/io_uring.h>
#include <kernel/asess.h>
#include <kernel/config.h>
#include <kernel/time.h>
#include <kernel/fs/ramfs.h>
#include <kernel/errno.h>

/* mmap flags */
#define MAP_SHARED    0x01
#define MAP_PRIVATE   0x02
#define MAP_FIXED     0x10
#define MAP_ANONYMOUS 0x20
#define MAP_GROWSDOWN 0x0100   /* stack mapping — pages fault in on demand (auto-grow) */
#define MAP_LOCKED    0x2000   /* lock+prefault the mapping (mmap-time mlock) */
#define MAP_POPULATE  0x8000   /* prefault hint — honoured lazily (best-effort) */
#define MAP_HUGETLB   0x40000
#define MAP_HUGE_SHIFT 26
#define MAP_HUGE_1GB  (30 << MAP_HUGE_SHIFT)

/* mmap prot */
#define PROT_READ  0x1
#define PROT_WRITE 0x2
#define PROT_EXEC  0x4

/* stat struct (kernel ABI) — layout differs between x86_64 and aarch64 */
#if defined(__x86_64__)
typedef struct {
	uint64_t st_dev;
	uint64_t st_ino;
	uint64_t st_nlink;   /* x86_64: nlink (8B) before mode */
	uint32_t st_mode;
	uint32_t st_uid;
	uint32_t st_gid;
	uint32_t __pad0;
	uint64_t st_rdev;
	int64_t  st_size;
	int64_t  st_blksize;
	int64_t  st_blocks;
	int64_t  st_atime_sec;
	uint64_t st_atime_nsec;
	int64_t  st_mtime_sec;
	uint64_t st_mtime_nsec;
	int64_t  st_ctime_sec;
	uint64_t st_ctime_nsec;
	uint64_t __unused[3];
} linux_stat_t;
#else /* aarch64: mode (4B) before nlink (4B), extra pad after rdev */
typedef struct {
	uint64_t st_dev;
	uint64_t st_ino;
	uint32_t st_mode;    /* aarch64: mode before nlink */
	uint32_t st_nlink;
	uint32_t st_uid;
	uint32_t st_gid;
	uint64_t st_rdev;
	uint64_t __pad1;
	int64_t  st_size;
	int32_t  st_blksize;
	int32_t  __pad2;
	int64_t  st_blocks;
	int64_t  st_atime_sec;
	uint64_t st_atime_nsec;
	int64_t  st_mtime_sec;
	uint64_t st_mtime_nsec;
	int64_t  st_ctime_sec;
	uint64_t st_ctime_nsec;
	uint32_t __unused4;
	uint32_t __unused5;
} linux_stat_t;
#endif

typedef struct {
	void  *iov_base;
	size_t iov_len;
} iovec_t;

/* ── helpers ─────────────────────────────────────────────── */

static int resolve_path(char *out, int dirfd, const char *upath);

/* read/write/open/stat syscalls → sys/syscall_io.c */
static long sys_write(int fd, const void *buf, size_t len);
static long sys_read(int fd, void *buf, size_t len);
static long sys_writev(int fd, const iovec_t *iov, int cnt);
static long sys_sendfile(int out_fd, int in_fd, long *offset, size_t count);
static long sys_readv(int fd, const iovec_t *iov, int cnt);
static long sys_open(const char *path, int flags, mode_t mode);
static long sys_openat(int dirfd, const char *path, int flags, mode_t mode);
static long sys_close(int fd);
static long sys_lseek(int fd, off_t off, int whence);
static long sys_fstat(int fd, linux_stat_t *st);

/* VMAs describe the address space, not a single thread, so they live on the
 * shared vm_space and every thread sees the same list (Linux keeps the VMA
 * tree in mm_struct). Because all threads share the list, every traversal and
 * mutation runs under the space's fault_lock — the same lock the page-fault
 * handler already holds while it walks the list — so a concurrent mmap/mprotect/
 * munmap on another CPU can't corrupt it mid-scan. The *_locked helpers assume
 * the caller holds it; the public entry points take it. A thread that lacks a
 * vm only during teardown has no regions to track. */
static void vma_add_locked(vm_space_t *vs, uint64_t vaddr, uint64_t size, uint32_t flags)
{
	vma_t *v = kmalloc(sizeof(*v));
	if (!v) return;
	v->vaddr = vaddr;
	v->size  = size;
	v->flags = flags;
	v->next  = vs->vmas;
	vs->vmas = v;
}

void vma_add(process_t *p, uint64_t vaddr, uint64_t size, uint32_t flags)
{
	if (!p->vm) return;
	uint64_t f = spin_lock_irq(&p->vm->fault_lock);
	vma_add_locked(p->vm, vaddr, size, flags);
	spin_unlock_irq(&p->vm->fault_lock, f);
}

static void vma_clip_locked(vm_space_t *vs, uint64_t va, uint64_t len)
{
	uint64_t end = va + len;
	int changed = 1;
	while (changed) {
		changed = 0;
		vma_t **pp = &vs->vmas;
		for (vma_t *v = *pp; v; pp = &v->next, v = v->next) {
			uint64_t vend = v->vaddr + v->size;
			if (v->vaddr >= end || vend <= va) continue;   /* disjoint */
			uint64_t lv = v->vaddr, ls = (va > v->vaddr) ? va - v->vaddr : 0;
			uint64_t rv = end,      rs = (vend > end)    ? vend - end    : 0;
			uint32_t fl = v->flags;
			*pp = v->next;
			kfree(v);
			if (ls) vma_add_locked(vs, lv, ls, fl);
			if (rs) vma_add_locked(vs, rv, rs, fl);
			changed = 1;   /* list mutated — restart the scan */
			break;
		}
	}
}

/* Remove [va, va+len) from the VMA set, trimming or splitting any overlapping
 * VMAs and re-adding the non-overlapping remainders. A fresh mmap (especially
 * MAP_FIXED, which ld.so uses to lay a library's segments over its initial
 * reservation) must replace whatever was there, not leave a stale VMA whose
 * permissions then win the fault lookup. */
void vma_clip(process_t *p, uint64_t va, uint64_t len)
{
	if (!p->vm) return;
	uint64_t f = spin_lock_irq(&p->vm->fault_lock);
	vma_clip_locked(p->vm, va, len);
	spin_unlock_irq(&p->vm->fault_lock, f);
}

/* Clip [va,va+len) and install one fresh VMA over it, atomically under the lock
 * so a concurrent fault never observes the gap between the clip and the add.
 * This is the install step every mmap variant uses. */
void vma_replace(process_t *p, uint64_t va, uint64_t len, uint32_t flags)
{
	if (!p->vm) return;
	uint64_t f = spin_lock_irq(&p->vm->fault_lock);
	vma_clip_locked(p->vm, va, len);
	vma_add_locked(p->vm, va, len, flags);
	spin_unlock_irq(&p->vm->fault_lock, f);
}

/* Free the whole VMA list of an address space. Called from vm_destroy when the
 * last thread drops its reference — the regions outlive any single thread. */
void vma_free_space(vm_space_t *vs)
{
	if (!vs) return;
	vma_t *v = vs->vmas;
	while (v) {
		vma_t *next = v->next;
		kfree(v);
		v = next;
	}
	vs->vmas = NULL;
}

/* exit/getpid → sys/syscall_proc.c; reboot/syslog/utimensat → sys/syscall_misc.c */

static uint64_t rng_next(void); /* defined below, used by execve + getrandom */

#define USER_STACK_TOP_   ARCH_USER_STACK_TOP
#define USER_STACK_PAGES_ ARCH_USER_STACK_PAGES
#define USER_MMAP_BASE_   ARCH_USER_MMAP_BASE

/* sys_execve → sys/syscall_proc.c */

/* sys_rt_sigreturn → sys/syscall_sig.c */

/* pipe2/fork/vfork/waitpid/arch_prctl → sys/syscall_proc.c */

/* prlimit64/sched_yield/sched_*affinity/prctl → sys/syscall_misc.c */

/* splitmix64 — fast, high-quality 64-bit PRNG (Sebastiano Vigna) */
static uint64_t rng_state = 0xdeadbeefcafebabe;

static uint64_t rng_next(void)
{
	rng_state += 0x9e3779b97f4a7c15ULL ^ kernel_ticks;
	uint64_t z = rng_state;
	z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
	z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
	return z ^ (z >> 31);
}

/* getrandom + statx (and linux_statx_t) → sys/syscall_misc.c */


/* memory-management syscalls (defined in sys/syscall_mm.c, included last) —
 * forward-declared so the other sys/*.c units can call them */
static long sys_brk(uint64_t addr);
static void drop_old_pages(process_t *p, uint64_t va, uint64_t len);
static long sys_mmap(uint64_t addr, size_t len, int prot, int flags, int fd, uint64_t off);
static long sys_munmap(uint64_t addr, size_t len);
static long sys_mprotect(uint64_t addr, size_t len, int prot);
static long sys_madvise(uint64_t addr, size_t len, int advice);
static long sys_msync(uint64_t addr, size_t len, int flags);
static long sys_mlock(uint64_t addr, size_t len);
static long sys_munlock(uint64_t addr, size_t len);
static long sys_mlock2(uint64_t addr, size_t len, int flags);
static long sys_mlockall(int flags);
static long sys_munlockall(void);

/* signal-management syscalls (defined in sys/syscall_sig.c) */
static long sys_rt_sigaction(int sig, const sigaction_t *act, sigaction_t *oldact, size_t sigsetsize);
static long sys_rt_sigprocmask(int how, const uint64_t *set, uint64_t *oldset, size_t sigsetsize);
static long sys_kill(pid_t pid, int sig);
static long kill_task(process_t *t, int sig);
static long sys_tgkill(pid_t tgid, pid_t tid, int sig);
static long sys_rt_sigqueueinfo(pid_t pid, int sig, const void *uinfo);
static long sys_rt_tgsigqueueinfo(pid_t tgid, pid_t tid, int sig, const void *uinfo);

#include "sys/syscall_fs.c"
#include "sys/syscall_proc.c"
#include "sys/syscall_timer.c"
#include "sys/syscall_io.c"
#include "sys/syscall_event.c"
#include "sys/syscall_net.c"
#include "sys/syscall_misc.c"
#include "sys/syscall_ptrace.c"
#include "sys/syscall_ipc.c"
#include "sys/syscall_mm.c"
#include "sys/syscall_sig.c"
#include "sys/syscall_compat.c"

/* SA_RESTART restarts a call a signal interrupted, except those that exist to wait
 * for a signal or a deadline: those report EINTR whatever the handler's flags. */
static int syscall_restartable(long n)
{
	switch (n) {
#ifdef SYS_ARCH_PAUSE
	case SYS_ARCH_PAUSE:
#endif
#ifdef SYS_ARCH_POLL
	case SYS_ARCH_POLL:
#endif
#ifdef SYS_ARCH_SELECT
	case SYS_ARCH_SELECT:
#endif
#ifdef SYS_ARCH_EPOLL_WAIT
	case SYS_ARCH_EPOLL_WAIT:
#endif
	case SYS_ARCH_PPOLL:
	case SYS_ARCH_PSELECT6:
	case SYS_ARCH_EPOLL_PWAIT:
	case SYS_ARCH_NANOSLEEP:
	case SYS_ARCH_CLOCK_NANOSLEEP:
	case SYS_ARCH_RT_SIGSUSPEND:
	case SYS_ARCH_RT_SIGTIMEDWAIT:
	case SYS_ARCH_SEMOP:
	case SYS_ARCH_MSGRCV:
	case SYS_ARCH_MSGSND:
		return 0;
	default:
		return 1;
	}
}

/* ── seccomp BPF (cBPF subset) ───────────────────────────────────────────────
 * Runs the installed classic-BPF programs over a seccomp_data record; the
 * action they return (SECCOMP_RET_*, with its data) is applied in syscall_gate. */
struct seccomp_data { uint32_t nr; uint32_t arch; uint64_t instruction_pointer; uint64_t args[6]; };
_Static_assert(sizeof(struct seccomp_data) == SECCOMP_DATA_SIZE, "seccomp_check bounds loads by this");

#if defined(__aarch64__)
#define SECCOMP_AUDIT_ARCH 0xc00000b7u   /* AUDIT_ARCH_AARCH64 */
#else
#define SECCOMP_AUDIT_ARCH 0xc000003eu   /* AUDIT_ARCH_X86_64 */
#endif

static long seccomp_action(uint32_t r)
{
	return (long)r;
}

/* An action's rank. Every filter runs and the most restrictive action wins, as
 * on Linux: KILL_PROCESS, KILL_THREAD, TRAP, ERRNO, USER_NOTIF, TRACE, LOG,
 * ALLOW. An action it does not know counts as KILL_PROCESS. */
static int seccomp_rank(uint32_t r)
{
	switch (r & 0xffff0000u) {
	case 0x00000000u: return 1;   /* KILL_THREAD */
	case 0x00030000u: return 2;   /* TRAP */
	case 0x00050000u: return 3;   /* ERRNO */
	case 0x7fc00000u: return 4;   /* USER_NOTIF */
	case 0x7ff00000u: return 5;   /* TRACE */
	case 0x7ffc0000u: return 6;   /* LOG */
	case 0x7fff0000u: return 7;   /* ALLOW */
	default:          return 0;   /* KILL_PROCESS, unknown */
	}
}

/* The 32-bit word at byte offset k of the record, 0 past its end. Copied, not
 * read through a uint32_t pointer: the record's fields are 64-bit, and a
 * mismatched-type load lets the compiler drop the stores that filled them. */
static int seccomp_load(const struct seccomp_data *d, uint32_t k, uint32_t *w)
{
	if ((size_t)k + 4 > sizeof(*d)) return 0;
	__builtin_memcpy(w, (const uint8_t *)d + k, 4);
	return 1;
}

/* A = A op s; 0 for a division by X == 0 (the program then kills, as on Linux) */
static int seccomp_alu(uint16_t code, uint32_t *A, uint32_t s)
{
	switch (BPF_OP(code)) {
	case BPF_ADD: *A += s; break;
	case BPF_SUB: *A -= s; break;
	case BPF_MUL: *A *= s; break;
	case BPF_DIV: if (!s) return 0; *A /= s; break;
	case BPF_OR:  *A |= s; break;
	case BPF_AND: *A &= s; break;
	case BPF_LSH: *A <<= (s & 31); break;
	case BPF_RSH: *A >>= (s & 31); break;
	case BPF_NEG: *A = -*A; break;
	case BPF_XOR: *A ^= s; break;
	default: return 0;
	}
	return 1;
}

/* Runs what seccomp_check let in (kernel/sys/syscall_misc.c). Jumps stay in
 * the program and loads in seccomp_data by that check; the bounds are kept
 * here too. pc and offsets are 64-bit so a huge k cannot wrap back into it. */
static long seccomp_run_filter(const struct seccomp_filter *flt, const struct seccomp_data *d)
{
	const struct sock_filter *f = flt->insns;
	uint32_t A = 0, X = 0, M[BPF_MEMWORDS] = { 0 };
	for (size_t pc = 0; pc < flt->len; pc++) {
		const struct sock_filter *i = &f[pc];
		uint32_t s = (i->code & BPF_X) ? X : i->k;
		if (BPF_CLASS(i->code) == BPF_CL_ALU) {
			if (!seccomp_alu(i->code, &A, s)) return 0;
			continue;
		}
		if (BPF_CLASS(i->code) == BPF_CL_JMP) {
			int t;
			switch (BPF_OP(i->code)) {
			case BPF_JA:   pc += i->k; continue;
			case BPF_JEQ:  t = A == s; break;
			case BPF_JGT:  t = A > s; break;
			case BPF_JGE:  t = A >= s; break;
			case BPF_JSET: t = (A & s) != 0; break;
			default:       return 0;
			}
			pc += t ? i->jt : i->jf;
			continue;
		}
		switch (i->code) {
		case BPF_LD_W_ABS:  if (!seccomp_load(d, i->k, &A)) return 0; break;
		case BPF_LD_W_LEN:  A = (uint32_t)sizeof(*d); break;
		case BPF_LDX_W_LEN: X = (uint32_t)sizeof(*d); break;
		case BPF_LD_IMM:    A = i->k; break;
		case BPF_LDX_IMM:   X = i->k; break;
		case BPF_LD_MEM:    if (i->k >= BPF_MEMWORDS) return 0; A = M[i->k]; break;
		case BPF_LDX_MEM:   if (i->k >= BPF_MEMWORDS) return 0; X = M[i->k]; break;
		case BPF_ST:        if (i->k >= BPF_MEMWORDS) return 0; M[i->k] = A; break;
		case BPF_STX:       if (i->k >= BPF_MEMWORDS) return 0; M[i->k] = X; break;
		case BPF_TAX:       X = A; break;
		case BPF_TXA:       A = X; break;
		case BPF_RET_K:     return seccomp_action(i->k);
		case BPF_RET_A:     return seccomp_action(A);
		default:            return 0;   /* unknown → kill */
		}
	}
	return 0;   /* fell off the end without RET → kill */
}

/* Every filter in the chain runs; the highest-ranked action wins, and among
 * equals the newest filter's (its data). */
static uint32_t seccomp_run_chain(process_t *p, uint64_t ip, long nr,
                                  long a0, long a1, long a2, long a3, long a4, long a5)
{
	struct seccomp_data d;
	d.nr = (uint32_t)nr; d.arch = SECCOMP_AUDIT_ARCH; d.instruction_pointer = ip;
	d.args[0]=(uint64_t)a0; d.args[1]=(uint64_t)a1; d.args[2]=(uint64_t)a2;
	d.args[3]=(uint64_t)a3; d.args[4]=(uint64_t)a4; d.args[5]=(uint64_t)a5;

	uint32_t act = 0x7fff0000u;   /* ALLOW */
	for (const struct seccomp_filter *f = p->seccomp_filter; f; f = f->prev) {
		uint32_t r = (uint32_t)seccomp_run_filter(f, &d);
		if (seccomp_rank(r) < seccomp_rank(act)) act = r;
	}
	return act;
}

/* seccomp: filter the syscall before dispatch. STRICT allows only
 * read/write/exit/sigreturn; FILTER runs the installed BPF program. A session
 * is gated first, so no filter a task installs can change the gate's answer.
 * 1: the syscall is not run, and *ret is its result. */
static int syscall_gate(process_t *sc, uint64_t ip, long n, const long *a, long *ret)
{
	if (sc->asess && !asess_syscall_allowed(sc, n, a[0], a[1], a[2])) {
		*ret = -1;   /* EPERM */
		return 1;
	} else if (sc->seccomp_mode == 1) {
		if (n != SYS_ARCH_READ && n != SYS_ARCH_WRITE && n != SYS_ARCH_EXIT &&
		    n != SYS_ARCH_EXIT_GROUP && n != SYS_ARCH_RT_SIGRETURN) {
			task_signal_group(sc, 9 /*SIGKILL*/);
			return 1;
		}
	} else if (sc->seccomp_mode == 2) {
		uint32_t r = seccomp_run_chain(sc, ip, n, a[0], a[1], a[2], a[3], a[4], a[5]);
		uint32_t data = r & 0xffffu;
		switch (r & 0xffff0000u) {
		case 0x7fff0000u:            /* ALLOW */
		case 0x7ffc0000u:            /* LOG */
			return 0;
		case 0x00050000u:            /* ERRNO: -data, at most MAX_ERRNO; 0 is success */
			*ret = -(long)(data > 4095 ? 4095 : data);
			return 1;
		case 0x00030000u:            /* TRAP: not run, rolled back, SIGSYS on the way out */
			*ret = CTX_SYSCALL_ROLLBACK(n, a[0]);
			task_force_seccomp(ip, (int)n, SECCOMP_AUDIT_ARCH, (int)data);
			return 1;
		case 0x7ff00000u:            /* TRACE, USER_NOTIF: no tracer or listener here */
		case 0x7fc00000u:
			*ret = -38;
			return 1;
		default:                     /* KILL_THREAD, KILL_PROCESS */
			task_signal_group(sc, 9);
			return 1;
		}
	}
	return 0;
}

/* Syscalls whose first argument is a fd. Their handlers look the file up and may
 * sleep with it, while a sibling thread can close the fd: the dispatcher holds a
 * reference for the whole call, so the file outlives every path through them. */
static int syscall_takes_fd0(long n)
{
	switch (n) {
	case SYS_ARCH_READ: case SYS_ARCH_WRITE: case SYS_ARCH_READV: case SYS_ARCH_WRITEV:
	case SYS_ARCH_LSEEK: case SYS_ARCH_IOCTL: case SYS_ARCH_FCNTL: case SYS_ARCH_FSTAT:
	case SYS_ARCH_GETDENTS64: case SYS_ARCH_FTRUNCATE: case SYS_ARCH_FCHMOD:
	case SYS_ARCH_FCHOWN: case SYS_ARCH_FSYNC: case SYS_ARCH_FDATASYNC:
	case SYS_ARCH_SYNC_FILE_RANGE: case SYS_ARCH_FCHDIR: case SYS_ARCH_FLOCK:
	case SYS_ARCH_FSTATFS: case SYS_ARCH_FGETXATTR: case SYS_ARCH_PREAD64:
	case SYS_ARCH_PWRITE64: case SYS_ARCH_SENDFILE: case SYS_ARCH_SENDTO:
	case SYS_ARCH_RECVFROM: case SYS_ARCH_SENDMSG: case SYS_ARCH_RECVMSG:
	case SYS_ARCH_SENDMMSG: case SYS_ARCH_RECVMMSG: case SYS_ARCH_CONNECT:
	case SYS_ARCH_BIND: case SYS_ARCH_LISTEN: case SYS_ARCH_ACCEPT: case SYS_ARCH_ACCEPT4:
	case SYS_ARCH_SHUTDOWN: case SYS_ARCH_GETSOCKNAME: case SYS_ARCH_GETPEERNAME:
	case SYS_ARCH_SETSOCKOPT: case SYS_ARCH_GETSOCKOPT: case SYS_ARCH_EPOLL_CTL:
	case SYS_ARCH_EPOLL_PWAIT: case SYS_ARCH_TIMERFD_SETTIME: case SYS_ARCH_TIMERFD_GETTIME:
	case SYS_ARCH_SIGNALFD4: case SYS_ARCH_INOTIFY_ADD_WATCH: case SYS_ARCH_INOTIFY_RM_WATCH:
	case SYS_ARCH_MQ_TIMEDSEND: case SYS_ARCH_MQ_TIMEDRECEIVE: case SYS_ARCH_MQ_GETSETATTR:
	case SYS_ARCH_SPLICE: case SYS_ARCH_COPY_FILE_RANGE: case SYS_ARCH_FALLOCATE:
	case SYS_ARCH_IO_URING_ENTER: case SYS_ARCH_IO_URING_REGISTER:
	case SYS_ARCH_PIDFD_SEND_SIGNAL: case SYS_ARCH_SETNS:
#if defined(__x86_64__)
	case SYS_ARCH_EPOLL_WAIT:
#endif
		return 1;
	default:
		return 0;
	}
}

/* ranix.slowsys on the command line: log each syscall that held its caller
 * longer than 150 ms, except the ones that exist to wait. A trace for "where
 * does this program spend its time" without stopping it under ptrace. */
int slowsys_on = -1;
uint64_t slowsys_ns(void) { int64_t s, ns; mono_time(&s, &ns); return (uint64_t)(s * 1000000000LL + ns); }
static int slowsys_waits(long n)
{
#if defined(__x86_64__)
	switch (n) { case 7: case 23: case 34: case 35: case 61: case 202: case 230: case 232:
	case 247: case 270: case 271: case 281: case 441: return 1; }
#endif
	return 0;
}

/* Syscall entry under slowsys: note the start time in *ss_s and *ss_ns, and log a
 * long stretch back in user mode since the last syscall. */
static void slowsys_enter(arch_context_t *ctx, int64_t *ss_s, int64_t *ss_ns)
{
	if (slowsys_on < 0) slowsys_on = cmdline_has("ranix.slowsys") ? 1 : 0;
	if (!slowsys_on) return;
	mono_time(ss_s, ss_ns);
	/* time back in user mode since the last syscall: CPU, faults or waiting to run */
	process_t *sc = task_current();
	uint64_t now = (uint64_t)(*ss_s * 1000000000LL + *ss_ns);
	if (sc->ss_exit_ns && now - sc->ss_exit_ns >= 500000000ULL)
		kprintf(KERN_INFO "slowsys: user pid=%u tid=%u %lu ms faults=%u in %lu ms prev=%d next=%ld\n",
		        sc->tgid, sc->pid, (unsigned long)((now - sc->ss_exit_ns) / 1000000), sc->ss_faults,
		        (unsigned long)(sc->ss_fault_ns / 1000000), sc->ss_prevnr, (long)CTX_SYSCALL_N(ctx));
}

/* Syscall exit under slowsys: log syscall n if it took too long. */
static void slowsys_exit(long n, long ret, const long *a, int64_t ss_s, int64_t ss_ns)
{
	if (!slowsys_on) return;
	/* a wait is logged only when it ran 500 ms or more, with its arguments */
	int w = slowsys_waits(n);
	int64_t e_s, e_ns; mono_time(&e_s, &e_ns);
	int64_t ms = (e_s - ss_s) * 1000 + (e_ns - ss_ns) / 1000000;
	if (ms >= (w ? 500 : 150))
		kprintf(KERN_INFO "slowsys: %s pid=%u tid=%u nr=%ld ret=%ld %ld ms a0=%lx a1=%lx a2=%lx a3=%lx\n",
		        w ? "wait" : "call", task_current()->tgid, task_current()->pid, n, ret, (long)ms,
		        a[0], a[1], a[2], a[3]);
	process_t *sc = task_current();
	sc->ss_exit_ns = (uint64_t)(e_s * 1000000000LL + e_ns); sc->ss_faults = 0; sc->ss_fault_ns = 0; sc->ss_prevnr = (int)n;
}

/* cgroup.freeze: the scheduler stops whoever it picks, so a task that never
 * sleeps here would run on. A tick catches one running in user mode. */
static void syscall_freeze_wait(void)
{
	process_t *cur = task_current();
	while (cgroup_frozen(cur->cgroup) && !(cur->sig_pending & (1UL << (SIGKILL - 1)))) {
		cur->in_freeze = 1;                            /* proc_wake_frozen ends it */
		task_sleep_if(cgroup_frozen(cur->cgroup));
		cur->in_freeze = 0;
	}
}

/* Flags clone(2) and clone3(2) act on (Linux values). */
#define CLONE_VM       0x00000100
#define CLONE_THREAD   0x00010000
#define CLONE_SETTLS   0x00080000
#define CLONE_PTID     0x00100000 /* CLONE_PARENT_SETTID */
#define CLONE_CTID     0x00200000 /* CLONE_CHILD_CLEARTID */
#define CLONE_CSETTID  0x01000000 /* CLONE_CHILD_SETTID */
#define CLONE_VFORK    0x00004000

/* clone(2): a vfork child, a thread sharing the address space, or a fork. */
static long syscall_clone(const long *a)
{
	uint64_t flags = (uint64_t)a[0];
	/* the child starts straight in iretq and fs_base goes through wrmsr: both
	 * fault in ring 0 on a non-canonical address */
	if (!access_ok((void *)a[1], 0)) return -EINVAL;
	if ((flags & CLONE_SETTLS) &&
#if defined(__aarch64__)
	    !access_ok((void *)a[3], 0)
#else
	    !access_ok((void *)a[4], 0)
#endif
	   ) return -EPERM;
	if (flags & CLONE_VFORK)
		return sys_vfork((uint64_t)a[1], flags);   /* a[1] = child stack, if any */
	if (flags & CLONE_VM) {
		/* Thread: share address space */
		uint64_t newsp  = (uint64_t)a[1];
		/* ABI: x86_64 clone(flags,newsp,ptid,ctid,tls)
		 *            aarch64 clone(flags,newsp,ptid,tls,ctid) */
		uint32_t *ptid  = (flags & CLONE_PTID)   ? (uint32_t*)a[2] : NULL;
#if defined(__aarch64__)
		uint64_t tls    = (flags & CLONE_SETTLS) ? (uint64_t)a[3] : 0;
		uint32_t *ctid_arg = (uint32_t*)a[4];
#else
		uint32_t *ctid_arg = (uint32_t*)a[3];
		uint64_t tls    = (flags & CLONE_SETTLS) ? (uint64_t)a[4] : 0;
#endif
		/* CLEARTID: clear+wake on exit (don't touch at creation).
		 * SETTID: write the tid into *ctid at creation. These are
		 * distinct flags sharing the one ctid argument. */
		uint32_t *clear_tid = (flags & CLONE_CTID)    ? ctid_arg : NULL;
		uint32_t *set_tid   = (flags & CLONE_CSETTID) ? ctid_arg : NULL;
		return (long)proc_clone_thread_ex(newsp, tls, clear_tid, ptid, set_tid, (flags & CLONE_THREAD) ? 0 : (int)((flags & 0xC00) | 1), (flags & CLONE_FS) != 0);
	}
	return (long)proc_fork(flags, (uint64_t)a[1]);   /* its own stack, if it gave one */
}

/* clone3(2), glibc 2.34+ thread creation. The struct clone_args prefix we use:
 * flags, pidfd, child_tid, parent_tid, exit_signal, stack, stack_size, tls. */
static long syscall_clone3(const long *a)
{
	struct { uint64_t flags, pidfd, child_tid, parent_tid, exit_signal, stack, stack_size, tls; } ca;
	memset(&ca, 0, sizeof(ca));
	size_t csz = (size_t)a[1] < sizeof(ca) ? (size_t)a[1] : sizeof(ca);
	if (copy_from_user(&ca, (void*)a[0], csz) != 0) return -EFAULT;
	uint64_t flags = ca.flags;
	uint64_t newsp = ca.stack + ca.stack_size;   /* clone3 gives stack base+size */
	if (!access_ok((void *)newsp, 0)) return -EINVAL;
	if ((flags & CLONE_SETTLS) && !access_ok((void *)ca.tls, 0)) return -EPERM;
	if (flags & CLONE_VFORK) {
		/* posix_spawn: a child sharing nothing but running on its own stack
		 * until it execs. CLONE_THREAD is what marks a real thread; VFORK
		 * without it is a spawn, so give it a private (COW) space + that stack. */
		return sys_vfork(newsp, flags);
	}
	if (flags & CLONE_VM) {
		uint32_t *ptid = (flags & CLONE_PTID) ? (uint32_t*)ca.parent_tid : NULL;
		uint64_t tls   = (flags & CLONE_SETTLS) ? ca.tls : 0;
		uint32_t *clear_tid = (flags & CLONE_CTID)    ? (uint32_t*)ca.child_tid : NULL;
		uint32_t *set_tid   = (flags & CLONE_CSETTID) ? (uint32_t*)ca.child_tid : NULL;
		return (long)proc_clone_thread_ex(newsp, tls, clear_tid, ptid, set_tid, (flags & CLONE_THREAD) ? 0 : (int)((flags & 0xC00) | 1), (flags & CLONE_FS) != 0);
	}
	return (long)proc_fork(flags, ca.stack ? newsp : 0);
}


/* Reading, writing, opening, file system calls and polling. 1: n was one of them and *ret holds its result. */
static int syscall_files(long n, const long *a, long *ret)
{
	switch (n) {
	/* ── I/O ─────────────────────────────────────────────── */
	case SYS_ARCH_READ:     *ret = sys_read((int)a[0], (void*)a[1], (size_t)a[2]); break;
	case SYS_ARCH_WRITE:    *ret = sys_write((int)a[0], (void*)a[1], (size_t)a[2]); break;
	case SYS_ARCH_SENDFILE: *ret = sys_sendfile((int)a[0], (int)a[1], (long*)a[2], (size_t)a[3]); break;
	case SYS_ARCH_READV:   *ret = sys_readv((int)a[0], (const iovec_t*)a[1], (int)a[2]); break;
	case SYS_ARCH_WRITEV:  *ret = sys_writev((int)a[0], (const iovec_t*)a[1], (int)a[2]); break;
	case SYS_ARCH_CLOSE:   *ret = sys_close((int)a[0]); break;
	case SYS_ARCH_LSEEK:   *ret = sys_lseek((int)a[0], (off_t)a[1], (int)a[2]); break;
	case SYS_ARCH_IOCTL:   *ret = sys_ioctl((int)a[0], (uint32_t)a[1], (uint64_t)a[2]); break;
	case SYS_ARCH_DUP:     *ret = sys_dup((int)a[0]); break;
	case SYS_ARCH_DUP3:    *ret = sys_dup3((int)a[0], (int)a[1], (int)a[2]); break;
	case SYS_ARCH_FCNTL:   *ret = sys_fcntl((int)a[0], (int)a[1], (uint64_t)a[2]); break;
	case SYS_ARCH_PIPE2:   *ret = sys_pipe2((int*)a[0], (int)a[1]); break;
#if defined(__x86_64__)
	case SYS_ARCH_PIPE:    *ret = sys_pipe2((int*)a[0], 0); break;
#endif
	case SYS_ARCH_EPOLL_CREATE1: *ret = sys_epoll_create1((int)a[0]); break;
	case SYS_ARCH_EPOLL_CTL:     *ret = sys_epoll_ctl((int)a[0], (int)a[1], (int)a[2], (linux_epoll_event_t*)a[3]); break;
	case SYS_ARCH_EPOLL_PWAIT:   *ret = sys_epoll_pwait((int)a[0], (linux_epoll_event_t*)a[1], (int)a[2], (int)a[3], (void*)a[4]); break;
	case SYS_ARCH_EPOLL_PWAIT2:  *ret = sys_epoll_pwait2((int)a[0], (linux_epoll_event_t*)a[1], (int)a[2], (const linux_timespec_t*)a[3], (void*)a[4]); break;
	/* ── open / stat ─────────────────────────────────────── */
#if defined(__x86_64__)
	case SYS_ARCH_OPEN:       *ret = sys_open((const char*)a[0], (int)a[1], (mode_t)a[2]); break;
#endif
	case SYS_ARCH_OPENAT:     *ret = sys_openat((int)a[0], (const char*)a[1], (int)a[2], (mode_t)a[3]); break;
	case SYS_ARCH_FSTAT:      *ret = sys_fstat((int)a[0], (linux_stat_t*)a[1]); break;
	case SYS_ARCH_NEWFSTATAT: *ret = sys_newfstatat((int)a[0], (const char*)a[1], (linux_stat_t*)a[2], (int)a[3]); break;
	case SYS_ARCH_GETDENTS64: *ret = sys_getdents64((int)a[0], (void*)a[1], (size_t)a[2]); break;
	/* ── fs ops ──────────────────────────────────────────── */
	case SYS_ARCH_CHDIR:    *ret = sys_chdir((const char*)a[0]); break;
	case SYS_ARCH_CHROOT:   *ret = sys_chroot((const char*)a[0]); break;
	case SYS_ARCH_PIVOT_ROOT: *ret = sys_pivot_root((const char*)a[0], (const char*)a[1]); break;
	case SYS_ARCH_GETCWD:   *ret = sys_getcwd((char*)a[0], (size_t)a[1]); break;
	case SYS_ARCH_MKDIRAT:  *ret = sys_mkdirat((int)a[0], (const char*)a[1], (mode_t)a[2]); break;
	case SYS_ARCH_UNLINKAT: *ret = sys_unlinkat((int)a[0], (const char*)a[1], (int)a[2]); break;
	case SYS_ARCH_RENAMEAT: *ret = sys_renameat((int)a[0], (const char*)a[1], (int)a[2], (const char*)a[3]); break;
	case SYS_ARCH_FTRUNCATE: *ret = sys_ftruncate((int)a[0], (off_t)a[1]); break;
	case SYS_ARCH_FCHMOD:   *ret = sys_fchmod((int)a[0], (mode_t)a[1]); break;
	case SYS_ARCH_FCHMODAT: *ret = sys_fchmodat((int)a[0], (const char*)a[1], (mode_t)a[2]); break;
	case SYS_ARCH_FCHOWN:   *ret = sys_fchown((int)a[0], (uid_t)a[1], (gid_t)a[2]); break;
	case SYS_ARCH_FCHOWNAT: *ret = sys_fchownat((int)a[0], (const char*)a[1], (uid_t)a[2], (gid_t)a[3], (int)a[4]); break;
	case SYS_ARCH_READLINKAT: *ret = sys_readlinkat_vfs((int)a[0], (const char*)a[1], (char*)a[2], (size_t)a[3]); break;
	case SYS_ARCH_SYMLINKAT: *ret = sys_symlinkat((const char*)a[0], (int)a[1], (const char*)a[2]); break;
	case SYS_ARCH_LINKAT:   *ret = sys_linkat((int)a[0], (const char*)a[1], (int)a[2], (const char*)a[3], 0); break;
	case SYS_ARCH_FACCESSAT:  *ret = sys_faccessat((int)a[0], (const char*)a[1], (int)a[2], (int)a[3]); break;
	case SYS_ARCH_FACCESSAT2: *ret = sys_faccessat((int)a[0], (const char*)a[1], (int)a[2], 0); break;
	/* ── poll / select ───────────────────────────────────── */
	case SYS_ARCH_PSELECT6: *ret = sys_pselect6((int)a[0], (uint64_t*)a[1], (uint64_t*)a[2], (uint64_t*)a[3], (linux_timespec_t*)a[4], (void*)a[5]); break;
	case SYS_ARCH_PPOLL:    *ret = sys_ppoll((linux_pollfd_t*)a[0], (uint64_t)a[1], (linux_timespec_t*)a[2], (void*)a[3]); break;
#if defined(__x86_64__)
	case SYS_ARCH_STAT:    *ret = sys_stat_path((const char*)a[0], (linux_stat_t*)a[1]); break;
	case SYS_ARCH_LSTAT:   *ret = sys_lstat_path((const char*)a[0], (linux_stat_t*)a[1]); break;
	case SYS_ARCH_ACCESS:  *ret = sys_access((const char*)a[0], (int)a[1]); break;
	case SYS_ARCH_SELECT:  *ret = sys_select((int)a[0], (uint64_t*)a[1], (uint64_t*)a[2], (uint64_t*)a[3], (linux_timeval_t*)a[4]); break;
	case SYS_ARCH_POLL:    *ret = sys_poll((linux_pollfd_t*)a[0], (uint64_t)a[1], (int)a[2]); break;
	case SYS_ARCH_DUP2:    *ret = sys_dup2((int)a[0], (int)a[1]); break;
	case SYS_ARCH_MKDIR:   *ret = sys_mkdir((const char*)a[0], (mode_t)a[1]); break;
	case SYS_ARCH_RMDIR:   *ret = sys_rmdir((const char*)a[0]); break;
	case SYS_ARCH_UNLINK:  *ret = sys_unlink((const char*)a[0]); break;
	case SYS_ARCH_LINK:    *ret = sys_linkat(AT_FDCWD, (const char*)a[0], AT_FDCWD, (const char*)a[1], 0); break;
	case SYS_ARCH_SYMLINK: *ret = sys_symlink((const char*)a[0], (const char*)a[1]); break;
	case SYS_ARCH_READLINK: *ret = sys_readlinkat_vfs(AT_FDCWD, (const char*)a[0], (char*)a[1], (size_t)a[2]); break;
	case SYS_ARCH_RENAME:  *ret = sys_rename((const char*)a[0], (const char*)a[1]); break;
	case SYS_ARCH_CHMOD:   *ret = sys_chmod((const char*)a[0], (mode_t)a[1]); break;
	case SYS_ARCH_CHOWN:   *ret = sys_chown((const char*)a[0], (uid_t)a[1], (gid_t)a[2]); break;
	case SYS_ARCH_TRUNCATE: *ret = sys_truncate((const char*)a[0], (off_t)a[1]); break;
	case SYS_ARCH_ARCH_PRCTL: *ret = sys_arch_prctl((int)a[0], (uint64_t)a[1]); break;
	case SYS_ARCH_EPOLL_CREATE: *ret = sys_epoll_create1(0); break;
	case SYS_ARCH_EPOLL_WAIT:   *ret = sys_epoll_pwait((int)a[0], (linux_epoll_event_t*)a[1], (int)a[2], (int)a[3], NULL); break;
#endif
	default:
		return 0;
	}
	return 1;
}

/* Memory, processes, threads, credentials, scheduling and signals. 1: n was one of them and *ret holds its result. */
static int syscall_process(arch_context_t *ctx, long n, const long *a, long *ret)
{
	switch (n) {
	/* ── memory ──────────────────────────────────────────── */
	case SYS_ARCH_BRK:     *ret = sys_brk((uint64_t)a[0]); break;
	case SYS_ARCH_MMAP:
		*ret = sys_mmap((uint64_t)a[0], (size_t)a[1], (int)a[2], (int)a[3], (int)a[4], (uint64_t)a[5]);
		break;
	case SYS_ARCH_MUNMAP:  *ret = sys_munmap((uint64_t)a[0], (size_t)a[1]); break;
	case SYS_ARCH_MPROTECT: *ret = sys_mprotect((uint64_t)a[0], (size_t)a[1], (int)a[2]); break;
	case SYS_ARCH_MBIND:         *ret = sys_mbind((int)a[2]); break;
	case SYS_ARCH_SET_MEMPOLICY: *ret = sys_set_mempolicy((int)a[0]); break;
	case SYS_ARCH_GET_MEMPOLICY: *ret = sys_get_mempolicy((int *)a[0], (unsigned long *)a[1], (unsigned long)a[2], (unsigned long)a[4]); break;
	case SYS_ARCH_MADVISE: *ret = sys_madvise((uint64_t)a[0], (size_t)a[1], (int)a[2]); break;
	case SYS_ARCH_MSYNC:   *ret = sys_msync((uint64_t)a[0], (size_t)a[1], (int)a[2]); break;
	case SYS_ARCH_PRLIMIT64: *ret = sys_prlimit64((pid_t)a[0], (int)a[1], (void*)a[2], (void*)a[3]); break;
	/* ── process ─────────────────────────────────────────── */
	case SYS_ARCH_EXIT:        *ret = sys_exit((int)a[0]); break;
	case SYS_ARCH_EXIT_GROUP:  *ret = sys_exit_group((int)a[0]); break;
	case SYS_ARCH_REBOOT:      *ret = sys_reboot((int)a[0], (int)a[1], (int)a[2], (void*)a[3]); break;
	case SYS_ARCH_SYSLOG:      *ret = sys_syslog((int)a[0], (char*)a[1], (int)a[2]); break;
	case SYS_ARCH_UTIMENSAT:   *ret = sys_utimensat((int)a[0], (const char*)a[1], (const void*)a[2], (int)a[3]); break;
	case SYS_ARCH_GETPID:      *ret = sys_getpid(); break;
	case SYS_ARCH_GETPPID:     *ret = sys_getppid(); break;
	case SYS_ARCH_GETUID:      *ret = sys_getuid(); break;
	case SYS_ARCH_GETEUID:     *ret = sys_geteuid(); break;
	case SYS_ARCH_GETGID:      *ret = sys_getgid(); break;
	case SYS_ARCH_GETEGID:     *ret = sys_getegid(); break;
	case SYS_ARCH_GETTID:      *ret = sys_gettid(); break;
	case SYS_ARCH_GETGROUPS:   *ret = sys_getgroups((int)a[0], (gid_t*)a[1]); break;
	case SYS_ARCH_SETGROUPS:   *ret = sys_setgroups((int)a[0], (const gid_t*)a[1]); break;
	case SYS_ARCH_GETRLIMIT:   *ret = sys_getrlimit((int)a[0], (void*)a[1]); break;
	case SYS_ARCH_SETRLIMIT:   *ret = sys_setrlimit((int)a[0], (const void*)a[1]); break;
	case SYS_ARCH_SETUID:      *ret = sys_setuid((uid_t)a[0]); break;
	case SYS_ARCH_SETGID:      *ret = sys_setgid((gid_t)a[0]); break;
	case SYS_ARCH_SETPGID:     *ret = sys_setpgid((pid_t)a[0], (pid_t)a[1]); break;
	case SYS_ARCH_GETPGID:     *ret = sys_getpgid((pid_t)a[0]); break;
	case SYS_ARCH_SETSID:      *ret = sys_setsid(); break;
	case SYS_ARCH_GETSID:      *ret = sys_getsid((pid_t)a[0]); break;
	case SYS_ARCH_GETPRIORITY: *ret = sys_getpriority((int)a[0], (int)a[1]); break;
	case SYS_ARCH_SETPRIORITY: *ret = sys_setpriority((int)a[0], (int)a[1], (int)a[2]); break;
#if defined(__aarch64__)
	case SYS_ARCH_SIGALTSTACK: *ret = sys_sigaltstack((const stack_t *)a[0], (stack_t *)a[1], ctx->sp); break;
#else
	case SYS_ARCH_SIGALTSTACK: *ret = sys_sigaltstack((const stack_t *)a[0], (stack_t *)a[1], ctx->rsp); break;
#endif
	case SYS_ARCH_UMASK:       *ret = sys_umask((mode_t)a[0]); break;
	case SYS_ARCH_SET_TID_ADDR:
		/* ABI: record the clear-on-exit address and return the tid. On
		 * thread exit the kernel writes 0 there and futex-wakes it. */
		task_current()->clear_child_tid = (uint32_t *)a[0];
		*ret = (long)task_vpid(task_current());
		break;
	case SYS_ARCH_KILL:        *ret = sys_kill((pid_t)a[0], (int)a[1]); break;
	case SYS_ARCH_TGKILL:     *ret = sys_tgkill((pid_t)a[0], (pid_t)a[1], (int)a[2]); break;
	case SYS_ARCH_RT_SIGQUEUEINFO:   *ret = sys_rt_sigqueueinfo((pid_t)a[0], (int)a[1], (const void*)a[2]); break;
	case SYS_ARCH_RT_TGSIGQUEUEINFO: *ret = sys_rt_tgsigqueueinfo((pid_t)a[0], (pid_t)a[1], (int)a[2], (const void*)a[3]); break;
	case SYS_ARCH_FUTEX:      *ret = sys_futex((uint32_t*)a[0], (int)a[1], (uint32_t)a[2], (linux_timespec_t*)a[3], (uint32_t*)a[4], (uint32_t)a[5]); break;
#if defined(__x86_64__)
	case SYS_ARCH_GETPGRP:    *ret = sys_getpgrp(); break;
#endif
	case SYS_ARCH_SCHED_YIELD: *ret = sys_sched_yield(); break;
	case SYS_ARCH_SCHED_SETAFFINITY: *ret = sys_sched_setaffinity((pid_t)a[0], (size_t)a[1], (void*)a[2]); break;
	case SYS_ARCH_SCHED_GETAFFINITY: *ret = sys_sched_getaffinity((pid_t)a[0], (size_t)a[1], (void*)a[2]); break;
	case SYS_ARCH_SCHED_SETSCHEDULER: *ret = sys_sched_setscheduler((pid_t)a[0], (int)a[1], (const void*)a[2]); break;
	case SYS_ARCH_SCHED_GETSCHEDULER: *ret = sys_sched_getscheduler((pid_t)a[0]); break;
	case SYS_ARCH_SCHED_GETATTR: *ret = sys_sched_getattr((pid_t)a[0], (void *)a[1], (unsigned int)a[2], (unsigned int)a[3]); break;
	case SYS_ARCH_SCHED_SETATTR: *ret = sys_sched_setattr((pid_t)a[0], (const void *)a[1], (unsigned int)a[2]); break;
	case SYS_ARCH_KCMP: *ret = sys_kcmp((pid_t)a[0], (pid_t)a[1], (int)a[2], (unsigned long)a[3], (unsigned long)a[4]); break;
	case SYS_ARCH_PROCESS_MRELEASE: *ret = sys_process_mrelease((int)a[0], (unsigned int)a[1]); break;
	case SYS_ARCH_SCHED_SETPARAM: *ret = sys_sched_setparam((pid_t)a[0], (const void*)a[1]); break;
	case SYS_ARCH_SCHED_GETPARAM: *ret = sys_sched_getparam((pid_t)a[0], (void*)a[1]); break;
	case SYS_ARCH_SCHED_GET_PRIORITY_MAX: *ret = sys_sched_get_priority_max((int)a[0]); break;
	case SYS_ARCH_SCHED_GET_PRIORITY_MIN: *ret = sys_sched_get_priority_min((int)a[0]); break;
	case SYS_ARCH_PRCTL:      *ret = sys_prctl((int)a[0], (uint64_t)a[1], (uint64_t)a[2], (uint64_t)a[3], (uint64_t)a[4]); break;
	case SYS_ARCH_GETRANDOM:  *ret = sys_getrandom((void*)a[0], (size_t)a[1], (unsigned int)a[2]); break;
	case SYS_ARCH_STATX:      *ret = sys_statx((int)a[0], (const char*)a[1], (int)a[2], (unsigned int)a[3], (linux_statx_t*)a[4]); break;
	case SYS_ARCH_MEMBARRIER: *ret = sys_membarrier((int)a[0], (unsigned int)a[1]); break;
	case SYS_ARCH_RSEQ:       *ret = -38; break;   /* ENOSYS: no rseq; glibc falls back */
	case SYS_ARCH_SET_ROBUST_LIST: *ret = sys_set_robust_list((void*)a[0], (size_t)a[1]); break;
	case SYS_ARCH_GET_ROBUST_LIST: *ret = sys_get_robust_list((pid_t)a[0], (void**)a[1], (size_t*)a[2]); break;
	case SYS_ARCH_RT_SIGACTION: *ret = sys_rt_sigaction((int)a[0], (const sigaction_t*)a[1], (sigaction_t*)a[2], (size_t)a[3]); break;
	case SYS_ARCH_RT_SIGPROCMASK: *ret = sys_rt_sigprocmask((int)a[0], (const uint64_t*)a[1], (uint64_t*)a[2], (size_t)a[3]); break;
	case SYS_ARCH_VFORK:  *ret = sys_vfork(0, 0); break;
#ifdef SYS_ARCH_FORK
	case SYS_ARCH_FORK:   *ret = sys_fork(); break;
#endif
	case SYS_ARCH_CLONE:  *ret = syscall_clone(a); break;
	case SYS_ARCH_UNSHARE: *ret = sys_unshare((unsigned long)a[0]); break;
	case SYS_ARCH_SETNS:   *ret = sys_setns((int)a[0], (int)a[1]); break;
	case SYS_ARCH_EXECVE: *ret = sys_execve((const char*)a[0], (char * const *)a[1], (char * const *)a[2]); break;
	case SYS_ARCH_EXECVEAT: *ret = sys_execveat((int)a[0], (const char*)a[1], (char * const *)a[2], (char * const *)a[3], (int)a[4]); break;
	case SYS_ARCH_PIDFD_OPEN: *ret = sys_pidfd_open((pid_t)a[0], (unsigned int)a[1]); break;
	case SYS_ARCH_PIDFD_SEND_SIGNAL: *ret = sys_pidfd_send_signal((int)a[0], (int)a[1], (void*)a[2], (unsigned int)a[3]); break;
	case SYS_ARCH_CLOSE_RANGE: *ret = sys_close_range((unsigned int)a[0], (unsigned int)a[1], (unsigned int)a[2]); break;
	case SYS_ARCH_SECCOMP: *ret = sys_seccomp((unsigned int)a[0], (unsigned int)a[1], (uint64_t)a[2]); break;
	case SYS_ARCH_ASESS:   *ret = sys_asess((unsigned int)a[0], (int)a[1], (void*)a[2], (size_t)a[3]); break;
	case SYS_ARCH_CLONE3: *ret = syscall_clone3(a); break;
	case SYS_ARCH_WAIT4:  *ret = sys_waitpid((pid_t)a[0], (int*)a[1], (int)a[2], (void*)a[3]); break;
	case SYS_ARCH_PROCESS_VM_READV:
		*ret = sys_process_vm((pid_t)a[0], (const iovec_t*)a[1], (unsigned long)a[2],
		                     (const iovec_t*)a[3], (unsigned long)a[4], (unsigned long)a[5], 0); break;
	case SYS_ARCH_PROCESS_VM_WRITEV:
		*ret = sys_process_vm((pid_t)a[0], (const iovec_t*)a[1], (unsigned long)a[2],
		                     (const iovec_t*)a[3], (unsigned long)a[4], (unsigned long)a[5], 1); break;
	case SYS_ARCH_FADVISE64: *ret = 0; break;   /* hint only — safe to ignore */
	default:
		return 0;
	}
	return 1;
}

/* Clocks and timers, sockets, and the remaining file, id and memory calls. 1: n was one of them and *ret holds its result. */
static int syscall_time_net(long n, const long *a, long *ret)
{
	switch (n) {
	/* ── time ────────────────────────────────────────────── */
	case SYS_ARCH_CLOCK_GETTIME: *ret = sys_clock_gettime((int)a[0], (linux_timespec_t*)a[1]); break;
	case SYS_ARCH_CLOCK_SETTIME: *ret = sys_clock_settime((int)a[0], (const linux_timespec_t*)a[1]); break;
	case SYS_ARCH_GETTIMEOFDAY:  *ret = sys_gettimeofday((linux_timeval_t*)a[0], (void*)a[1]); break;
	case SYS_ARCH_CLOCK_GETRES:  *ret = sys_clock_getres((int)a[0], (linux_timespec_t*)a[1]); break;
	case SYS_ARCH_ADJTIMEX:      *ret = sys_adjtimex((k_timex_t*)a[0]); break;
#ifdef SYS_ARCH_SETTIMEOFDAY
	case SYS_ARCH_SETTIMEOFDAY:  *ret = sys_settimeofday((const linux_timeval_t*)a[0], (const void*)a[1]); break;
#endif
	case SYS_ARCH_NANOSLEEP:     *ret = sys_nanosleep((linux_timespec_t*)a[0], (linux_timespec_t*)a[1]); break;
	case SYS_ARCH_GETITIMER:     *ret = sys_getitimer((int)a[0], (itimerval_t*)a[1]); break;
	case SYS_ARCH_SETITIMER:     *ret = sys_setitimer((int)a[0], (const itimerval_t*)a[1], (itimerval_t*)a[2]); break;
	case SYS_ARCH_TIMES:         *ret = sys_times((void*)a[0]); break;
	/* ── misc ────────────────────────────────────────────── */
	case SYS_ARCH_UNAME:   *ret = sys_uname((linux_utsname_t*)a[0]); break;
	case SYS_ARCH_SYSINFO: *ret = sys_sysinfo((void*)a[0]); break;
	/* ── sockets ─────────────────────────────────────────── */
	case SYS_ARCH_SOCKET:      *ret = sys_socket((int)a[0], (int)a[1], (int)a[2]); break;
	case SYS_ARCH_BIND:        *ret = sys_bind((int)a[0], (const void*)a[1], (uint32_t)a[2]); break;
	case SYS_ARCH_CONNECT:     *ret = sys_connect((int)a[0], (const void*)a[1], (uint32_t)a[2]); break;
	case SYS_ARCH_LISTEN:      *ret = sys_listen((int)a[0], (int)a[1]); break;
	case SYS_ARCH_ACCEPT:      *ret = sys_accept4((int)a[0], (void*)a[1], (uint32_t*)a[2], 0); break;
	case SYS_ARCH_ACCEPT4:     *ret = sys_accept4((int)a[0], (void*)a[1], (uint32_t*)a[2], (int)a[3]); break;
	case SYS_ARCH_SENDTO:      *ret = sys_sendto((int)a[0], (void*)a[1], (size_t)a[2], (int)a[3], (void*)a[4], (uint32_t)a[5]); break;
	case SYS_ARCH_RECVFROM:    *ret = sys_recvfrom((int)a[0], (void*)a[1], (size_t)a[2], (int)a[3], (void*)a[4], (uint32_t*)a[5]); break;
	case SYS_ARCH_SENDMSG:     *ret = sys_sendmsg((int)a[0], (const void*)a[1], (int)a[2]); break;
	case SYS_ARCH_RECVMSG:     *ret = sys_recvmsg((int)a[0], (void*)a[1], (int)a[2]); break;
	case SYS_ARCH_SENDMMSG:    *ret = sys_sendmmsg((int)a[0], (void*)a[1], (uint32_t)a[2], (int)a[3]); break;
	case SYS_ARCH_RECVMMSG:    *ret = sys_recvmmsg((int)a[0], (void*)a[1], (uint32_t)a[2], (int)a[3]); break;
	case SYS_ARCH_GETSOCKNAME: *ret = sys_getsockname((int)a[0], (void*)a[1], (uint32_t*)a[2]); break;
	case SYS_ARCH_GETPEERNAME: *ret = sys_getpeername((int)a[0], (void*)a[1], (uint32_t*)a[2]); break;
	case SYS_ARCH_SETSOCKOPT:  *ret = sys_setsockopt((int)a[0], (int)a[1], (int)a[2], (void*)a[3], (uint32_t)a[4]); break;
	case SYS_ARCH_GETSOCKOPT:  *ret = sys_getsockopt((int)a[0], (int)a[1], (int)a[2], (void*)a[3], (uint32_t*)a[4]); break;
	case SYS_ARCH_SHUTDOWN:    *ret = sys_shutdown((int)a[0], (int)a[1]); break;
	/* ── missing batch ───────────────────────────────────────── */
	case SYS_ARCH_PREAD64:     *ret = sys_pread64((int)a[0], (void*)a[1], (size_t)a[2], (off_t)a[3]); break;
	case SYS_ARCH_PWRITE64:    *ret = sys_pwrite64((int)a[0], (void*)a[1], (size_t)a[2], (off_t)a[3]); break;
	case SYS_ARCH_MREMAP:      *ret = sys_mremap((uint64_t)a[0], (size_t)a[1], (size_t)a[2], (int)a[3], (uint64_t)a[4]); break;
	case SYS_ARCH_FLOCK:       *ret = sys_flock((int)a[0], (int)a[1]); break;
	case SYS_ARCH_FSYNC:       *ret = sys_fsync((int)a[0]); break;
	case SYS_ARCH_FDATASYNC:   *ret = sys_fdatasync((int)a[0]); break;
	case SYS_ARCH_SYNC_FILE_RANGE: *ret = sys_sync_file_range((int)a[0], (off_t)a[1], (off_t)a[2], (unsigned int)a[3]); break;
	case SYS_ARCH_FCHDIR:      *ret = sys_fchdir((int)a[0]); break;
	case SYS_ARCH_GETRUSAGE:   *ret = sys_getrusage((int)a[0], (void*)a[1]); break;
	case SYS_ARCH_SETREUID:    *ret = sys_setreuid((uid_t)a[0], (uid_t)a[1]); break;
	case SYS_ARCH_SETREGID:    *ret = sys_setregid((gid_t)a[0], (gid_t)a[1]); break;
	case SYS_ARCH_SETRESUID:   *ret = sys_setresuid((uid_t)a[0], (uid_t)a[1], (uid_t)a[2]); break;
	case SYS_ARCH_GETRESUID:   *ret = sys_getresuid((uid_t*)a[0], (uid_t*)a[1], (uid_t*)a[2]); break;
	case SYS_ARCH_SETRESGID:   *ret = sys_setresgid((gid_t)a[0], (gid_t)a[1], (gid_t)a[2]); break;
	case SYS_ARCH_GETRESGID:   *ret = sys_getresgid((gid_t*)a[0], (gid_t*)a[1], (gid_t*)a[2]); break;
	case SYS_ARCH_SETFSUID:    *ret = sys_setfsuid((uid_t)a[0]); break;
	case SYS_ARCH_SETFSGID:    *ret = sys_setfsgid((gid_t)a[0]); break;
	case SYS_ARCH_PERSONALITY: *ret = sys_personality((unsigned int)a[0]); break;
	case SYS_ARCH_GETCPU:      *ret = sys_getcpu((unsigned int*)a[0], (unsigned int*)a[1], (void*)a[2]); break;
	case SYS_ARCH_RT_SIGPENDING: *ret = sys_rt_sigpending((uint64_t*)a[0], (size_t)a[1]); break;
	case SYS_ARCH_RT_SIGSUSPEND: *ret = sys_rt_sigsuspend((const uint64_t*)a[0], (size_t)a[1]); break;
	case SYS_ARCH_MLOCK:       *ret = sys_mlock((uint64_t)a[0], (size_t)a[1]); break;
	case SYS_ARCH_MUNLOCK:     *ret = sys_munlock((uint64_t)a[0], (size_t)a[1]); break;
	case SYS_ARCH_MLOCK2:      *ret = sys_mlock2((uint64_t)a[0], (size_t)a[1], (int)a[2]); break;
	case SYS_ARCH_MLOCKALL:    *ret = sys_mlockall((int)a[0]); break;
	case SYS_ARCH_MUNLOCKALL:  *ret = sys_munlockall(); break;
	case SYS_ARCH_SYNC:        *ret = sys_sync(); break;
	case SYS_ARCH_MOUNT:       *ret = sys_mount((const char*)a[0], (const char*)a[1], (const char*)a[2], (unsigned long)a[3], (const void*)a[4]); break;
	case SYS_ARCH_UMOUNT2:     *ret = sys_umount2((const char*)a[0], (int)a[1]); break;
	case SYS_ARCH_SETHOSTNAME: *ret = sys_sethostname((const char*)a[0], (size_t)a[1]); break;
	case SYS_ARCH_RT_SIGTIMEDWAIT: *ret = sys_rt_sigtimedwait((const uint64_t *)a[0], (void *)a[1], (const linux_timespec_t *)a[2], (size_t)a[3]); break;
	case SYS_ARCH_TKILL:       *ret = sys_tkill((pid_t)a[0], (int)a[1]); break;
	case SYS_ARCH_CLOCK_NANOSLEEP: *ret = sys_clock_nanosleep((int)a[0], (int)a[1], (linux_timespec_t*)a[2], (linux_timespec_t*)a[3]); break;
	case SYS_ARCH_WAITID:      *ret = sys_waitid((int)a[0], (int)a[1], (void*)a[2], (int)a[3], (void*)a[4]); break;
	case SYS_ARCH_SPLICE:      *ret = sys_splice((int)a[0], (off_t*)a[1], (int)a[2], (off_t*)a[3], (size_t)a[4], (unsigned int)a[5]); break;
	case SYS_ARCH_FALLOCATE:   *ret = sys_fallocate((int)a[0], (int)a[1], (off_t)a[2], (off_t)a[3]); break;
	case SYS_ARCH_COPY_FILE_RANGE: *ret = sys_copy_file_range((int)a[0], (off_t*)a[1], (int)a[2], (off_t*)a[3], (size_t)a[4], (unsigned int)a[5]); break;
	case SYS_ARCH_SOCKETPAIR:  *ret = sys_socketpair((int)a[0], (int)a[1], (int)a[2], (int*)a[3]); break;
	default:
		return 0;
	}
	return 1;
}

/* Event fds, io_uring, arch-only calls, statfs, xattrs, ptrace and IPC. 1: n was one of them and *ret holds its result. */
static int syscall_events_ipc(long n, const long *a, long *ret)
{
	switch (n) {
	/* ── fd-based notifications ──────────────────────────────── */
#if defined(__x86_64__)
	case SYS_ARCH_EVENTFD:
#endif
	case SYS_ARCH_EVENTFD2:    *ret = sys_eventfd2((unsigned int)a[0], (int)a[1]); break;
	case SYS_ARCH_PREADV:     *ret = sys_preadv((int)a[0], (const iovec_t *)a[1], (int)a[2], (off_t)a[3]); break;
	case SYS_ARCH_PWRITEV:    *ret = sys_pwritev((int)a[0], (const iovec_t *)a[1], (int)a[2], (off_t)a[3]); break;
	case SYS_ARCH_PREADV2:    *ret = sys_preadv2((int)a[0], (const iovec_t *)a[1], (int)a[2], (off_t)a[3]); break;
	case SYS_ARCH_PWRITEV2:   *ret = sys_pwritev2((int)a[0], (const iovec_t *)a[1], (int)a[2], (off_t)a[3]); break;
	case SYS_ARCH_MINCORE:    *ret = sys_mincore((uint64_t)a[0], (size_t)a[1], (uint8_t *)a[2]); break;
	case SYS_ARCH_READAHEAD:  *ret = sys_readahead((int)a[0], (off_t)a[1], (size_t)a[2]); break;
	case SYS_ARCH_SYNCFS:     *ret = sys_syncfs((int)a[0]); break;
	case SYS_ARCH_FSETXATTR:  *ret = sys_fsetxattr((int)a[0], (const char *)a[1], (const void *)a[2], (size_t)a[3], (int)a[4]); break;
	case SYS_ARCH_FLISTXATTR: *ret = sys_flistxattr((int)a[0], (char *)a[1], (size_t)a[2]); break;
	case SYS_ARCH_LREMOVEXATTR: *ret = sys_removexattr((const char *)a[0], (const char *)a[1]); break;
	case SYS_ARCH_FREMOVEXATTR: *ret = sys_fremovexattr((int)a[0], (const char *)a[1]); break;
	case SYS_ARCH_SCHED_RR_GET_INTERVAL: *ret = sys_sched_rr_get_interval((pid_t)a[0], (int64_t *)a[1]); break;
	case SYS_ARCH_SETDOMAINNAME: *ret = sys_setdomainname((const char *)a[0], (size_t)a[1]); break;
	case SYS_ARCH_IOPRIO_SET: *ret = 0; break;
	case SYS_ARCH_IOPRIO_GET: *ret = 0; break;
	case SYS_ARCH_PKEY_ALLOC: *ret = sys_pkey_alloc((unsigned long)a[0], (unsigned long)a[1]); break;
	case SYS_ARCH_PKEY_FREE:  *ret = -22; break;
	case SYS_ARCH_PKEY_MPROTECT: *ret = sys_pkey_mprotect((uint64_t)a[0], (size_t)a[1], (int)a[2], (int)a[3]); break;
	case SYS_ARCH_OPENAT2:    *ret = sys_openat2((int)a[0], (const char *)a[1], (const void *)a[2], (size_t)a[3]); break;
	case SYS_ARCH_FCHMODAT2:  *ret = sys_fchmodat2((int)a[0], (const char *)a[1], (mode_t)a[2], (int)a[3]); break;
	case SYS_ARCH_RESTART_SYSCALL: *ret = -4; break;
#ifdef SYS_ARCH_UTIME
	case SYS_ARCH_UTIME:      *ret = sys_utime((const char *)a[0], (const int64_t *)a[1]); break;
	case SYS_ARCH_UTIMES:     *ret = sys_utimes((const char *)a[0], (const ktv_t *)a[1]); break;
	case SYS_ARCH_FUTIMESAT:  *ret = sys_futimesat((int)a[0], (const char *)a[1], (const ktv_t *)a[2]); break;
	case SYS_ARCH_SIGNALFD:   *ret = sys_signalfd4((int)a[0], (const uint64_t *)a[1], (size_t)a[2], 0); break;
#endif
	case SYS_ARCH_TIMER_CREATE:     *ret = sys_timer_create((int)a[0], (const void *)a[1], (int *)a[2]); break;
	case SYS_ARCH_TIMER_SETTIME:    *ret = sys_timer_settime((int)a[0], (int)a[1], (const kits_t *)a[2], (kits_t *)a[3]); break;
	case SYS_ARCH_TIMER_GETTIME:    *ret = sys_timer_gettime((int)a[0], (kits_t *)a[1]); break;
	case SYS_ARCH_TIMER_GETOVERRUN: *ret = sys_timer_getoverrun((int)a[0]); break;
	case SYS_ARCH_TIMER_DELETE:     *ret = sys_timer_delete((int)a[0]); break;
	case SYS_ARCH_TIMERFD_CREATE:  *ret = sys_timerfd_create((int)a[0], (int)a[1]); break;
	case SYS_ARCH_TIMERFD_SETTIME: *ret = sys_timerfd_settime((int)a[0], (int)a[1], (linux_itimerspec_t*)a[2], (linux_itimerspec_t*)a[3]); break;
	case SYS_ARCH_TIMERFD_GETTIME: *ret = sys_timerfd_gettime((int)a[0], (linux_itimerspec_t*)a[1]); break;
#if defined(__x86_64__)
	case SYS_ARCH_INOTIFY_INIT:
#endif
	case SYS_ARCH_INOTIFY_INIT1:      *ret = sys_inotify_init1((int)a[0]); break;
	case SYS_ARCH_INOTIFY_ADD_WATCH:  *ret = sys_inotify_add_watch((int)a[0], (const char*)a[1], (uint32_t)a[2]); break;
	case SYS_ARCH_INOTIFY_RM_WATCH:   *ret = sys_inotify_rm_watch((int)a[0], (int)a[1]); break;
	case SYS_ARCH_SIGNALFD4:   *ret = sys_signalfd4((int)a[0], (const uint64_t*)a[1], (size_t)a[2], (int)a[3]); break;
	case SYS_ARCH_MEMFD_CREATE: *ret = sys_memfd_create((const char*)a[0], (unsigned int)a[1]); break;
	/* ── io_uring ────────────────────────────────────────────── */
	case SYS_ARCH_IO_URING_SETUP:    *ret = io_uring_setup((uint32_t)a[0], (struct io_uring_params*)a[1]); break;
	case SYS_ARCH_IO_URING_ENTER:    *ret = io_uring_enter((int)a[0], (uint32_t)a[1], (uint32_t)a[2], (uint32_t)a[3], (const void*)a[4], (size_t)a[5]); break;
	case SYS_ARCH_IO_URING_REGISTER: *ret = io_uring_register((int)a[0], (unsigned int)a[1], (void*)a[2], (unsigned int)a[3]); break;
	/* ── arch-specific ───────────────────────────────────────── */
#if defined(__x86_64__)
	case SYS_ARCH_PAUSE:       *ret = sys_pause(); break;
	case SYS_ARCH_ALARM:       *ret = sys_alarm((unsigned int)a[0]); break;
	case SYS_ARCH_CREAT:       *ret = sys_creat((const char*)a[0], (mode_t)a[1]); break;
	case SYS_ARCH_LCHOWN:      *ret = sys_lchown((const char*)a[0], (uid_t)a[1], (gid_t)a[2]); break;
	case SYS_ARCH_MKNOD:       *ret = sys_mknod((const char*)a[0], (mode_t)a[1], (uint64_t)a[2]); break;
	case SYS_ARCH_MKNODAT:     *ret = sys_mknodat((int)a[0], (const char*)a[1], (mode_t)a[2], (uint64_t)a[3]); break;
	case SYS_ARCH_TIME_SYSCALL: *ret = sys_time_syscall((long*)a[0]); break;
	case SYS_ARCH_RENAMEAT2:   *ret = sys_renameat2((int)a[0], (const char*)a[1], (int)a[2], (const char*)a[3], (unsigned int)a[4]); break;
#else
	case SYS_ARCH_MKNODAT:     *ret = sys_mknodat((int)a[0], (const char*)a[1], (mode_t)a[2], (uint64_t)a[3]); break;
	case SYS_ARCH_RENAMEAT2:   *ret = sys_renameat2((int)a[0], (const char*)a[1], (int)a[2], (const char*)a[3], (unsigned int)a[4]); break;
#endif
	case SYS_ARCH_STATFS:    *ret = sys_statfs((const char*)a[0], (kernel_statfs_t*)a[1]); break;
	case SYS_ARCH_FSTATFS:   *ret = sys_fstatfs((int)a[0], (kernel_statfs_t*)a[1]); break;
	case SYS_ARCH_CAPGET:    *ret = sys_capget((void*)a[0], (void*)a[1]); break;
	case SYS_ARCH_CAPSET:    *ret = sys_capset((void*)a[0], (const void*)a[1]); break;
	case SYS_ARCH_GETXATTR:  *ret = sys_getxattr((const char*)a[0],(const char*)a[1],(void*)a[2],(size_t)a[3]); break;
	case SYS_ARCH_LGETXATTR: *ret = sys_lgetxattr((const char*)a[0],(const char*)a[1],(void*)a[2],(size_t)a[3]); break;
	case SYS_ARCH_FGETXATTR: *ret = sys_fgetxattr((int)a[0],(const char*)a[1],(void*)a[2],(size_t)a[3]); break;
	case SYS_ARCH_SETXATTR:  *ret = sys_setxattr((const char*)a[0],(const char*)a[1],(const void*)a[2],(size_t)a[3],(int)a[4]); break;
	case SYS_ARCH_LSETXATTR: *ret = sys_lsetxattr((const char*)a[0],(const char*)a[1],(const void*)a[2],(size_t)a[3],(int)a[4]); break;
	case SYS_ARCH_LISTXATTR: *ret = sys_listxattr((const char*)a[0],(char*)a[1],(size_t)a[2]); break;
	case SYS_ARCH_LLISTXATTR:*ret = sys_llistxattr((const char*)a[0],(char*)a[1],(size_t)a[2]); break;
	case SYS_ARCH_REMOVEXATTR:*ret = sys_removexattr((const char*)a[0],(const char*)a[1]); break;
	case SYS_ARCH_PTRACE:    *ret = sys_ptrace((long)a[0], (pid_t)a[1], (uint64_t)a[2], (uint64_t)a[3]); break;
	/* SysV IPC */
	case SYS_ARCH_SHMGET:   *ret = sys_shmget((key_t)a[0], (size_t)a[1], (int)a[2]); break;
	case SYS_ARCH_SHMAT:    *ret = sys_shmat((int)a[0], (const void*)a[1], (int)a[2]); break;
	case SYS_ARCH_SHMDT:    *ret = sys_shmdt((const void*)a[0]); break;
	case SYS_ARCH_SHMCTL:   *ret = sys_shmctl((int)a[0], (int)a[1], (void*)a[2]); break;
	case SYS_ARCH_SEMGET:   *ret = sys_semget((key_t)a[0], (int)a[1], (int)a[2]); break;
	case SYS_ARCH_SEMOP:    *ret = sys_semop((int)a[0], (const sembuf_t*)a[1], (size_t)a[2]); break;
	case SYS_ARCH_SEMCTL:   *ret = sys_semctl((int)a[0], (int)a[1], (int)a[2], (uint64_t)a[3]); break;
	case SYS_ARCH_MSGGET:   *ret = sys_msgget((key_t)a[0], (int)a[1]); break;
	case SYS_ARCH_MSGSND:   *ret = sys_msgsnd((int)a[0], (const void*)a[1], (size_t)a[2], (int)a[3]); break;
	case SYS_ARCH_MSGRCV:   *ret = sys_msgrcv((int)a[0], (void*)a[1], (size_t)a[2], (long)a[3], (int)a[4]); break;
	case SYS_ARCH_MSGCTL:   *ret = sys_msgctl((int)a[0], (int)a[1], (void*)a[2]); break;
	/* POSIX IPC */
	case SYS_ARCH_MQ_OPEN:         *ret = sys_mq_open((const char*)a[0],(int)a[1],(mode_t)a[2],(mq_attr_t*)a[3]); break;
	case SYS_ARCH_MQ_UNLINK:       *ret = sys_mq_unlink((const char*)a[0]); break;
	case SYS_ARCH_MQ_TIMEDSEND:    *ret = sys_mq_timedsend((int)a[0],(const char*)a[1],(size_t)a[2],(uint32_t)a[3],(const void*)a[4]); break;
	case SYS_ARCH_MQ_TIMEDRECEIVE: *ret = sys_mq_timedreceive((int)a[0],(char*)a[1],(size_t)a[2],(uint32_t*)a[3],(const void*)a[4]); break;
	case SYS_ARCH_MQ_GETSETATTR:   *ret = sys_mq_getsetattr((int)a[0],(const mq_attr_t*)a[1],(mq_attr_t*)a[2]); break;
	default:
		return 0;
	}
	return 1;
}

/* The syscall entry from both arches: gate, dispatch by number, then the
 * bookkeeping every return to user mode needs. */
void syscall_handle(arch_context_t *ctx)
{
	int64_t ss_s = 0, ss_ns = 0;
	slowsys_enter(ctx, &ss_s, &ss_ns);
	arch_get_cpu_ctx()->wd_where = "syscall";
	/* Kernel-stack canary on trap entry too (not just on the way back out in
	 * signal_dispatch): catch an overflow from the previous kernel path before
	 * this syscall builds more frames on a already-clobbered stack. */
	kstack_check();
	{ process_t *cur = task_current(); if (cur) { cur->uctx = ctx; cur->in_syscall = 1; cur->last_syscall = (int)CTX_SYSCALL_N(ctx); } }   /* ptrace task_pt_regs: syscalls always enter from user */
	long n    = (long)CTX_SYSCALL_N(ctx);
	long arg0 = (long)CTX_SYSCALL_ARG0(ctx);
	long arg1 = (long)CTX_SYSCALL_ARG1(ctx);
	long arg2 = (long)CTX_SYSCALL_ARG2(ctx);
	long arg3 = (long)CTX_SYSCALL_ARG3(ctx);
	long arg4 = (long)CTX_SYSCALL_ARG4(ctx);
	long arg5 = (long)CTX_SYSCALL_ARG5(ctx);
	long ret  = -ENOSYS;

	task_current()->ctx = ctx;
	task_current()->walk_denied = 0;
	ptrace_stop_check(ctx, 0);

	long sargs[6] = { arg0, arg1, arg2, arg3, arg4, arg5 };
	int sec_skip = syscall_gate(task_current(), CTX_SYSCALL_IP(ctx), n, sargs, &ret);

	vfs_file_t *fd0_pin = (!sec_skip && syscall_takes_fd0(n))
	                    ? fget(task_current()->fds, (int)arg0) : NULL;

	if (!sec_skip && n == SYS_ARCH_RT_SIGRETURN) {
		/* returns directly (no ret value), but must still clear in_syscall or
		 * CPU-time accounting stays stuck in "kernel" */
		sys_rt_sigreturn(ctx); task_current()->in_syscall = 0; return;
	}
	if (!sec_skip && !syscall_files(n, sargs, &ret) && !syscall_process(ctx, n, sargs, &ret) &&
	    !syscall_time_net(n, sargs, &ret) && !syscall_events_ipc(n, sargs, &ret)) {
		kprintf(KERN_WARN "syscall: unimplemented %ld (pid=%u)\n",
		        n, task_current()->pid);
		ret = -ENOSYS;
	}

	if (fd0_pin) fput(fd0_pin);

	/* Every caller reports a failed walk as ENOENT. A session keeps that answer:
	 * what it may not search is simply not in its view. */
	if (ret == -ENOENT && task_current()->walk_denied && !task_current()->asess) ret = -EACCES;

	vfs_walk_done(task_current());
	slowsys_exit(n, ret, sargs, ss_s, ss_ns);
	CTX_SYSCALL_RET(ctx, ret);
	task_current()->ctx = ctx;
	if (ret == -EINTR && syscall_restartable(n)) {
		process_t *cur = task_current();
		cur->sys_intr = 1;
		cur->sys_arg0 = (uint64_t)arg0;
	}
	syscall_freeze_wait();
	task_current()->in_syscall = 0;
	ptrace_stop_check(ctx, 1);
}
