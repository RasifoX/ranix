#include <kernel/syscall.h>
#include <kernel/capabilities.h>
#include <kernel/proc.h>
#include <kernel/nsproxy.h>
#include <kernel/elf.h>
#include <kernel/vfs.h>
#include <kernel/hal.h>
#include <kernel/kprintf.h>
#include <kernel/mm/buddy.h>
#include <kernel/mm/pmm.h>
#include <kernel/mm/heap.h>
#include <kernel/mm/mmu.h>
#include <kernel/mm/vm.h>
#include <kernel/lib/string.h>
#include <kernel/sync.h>
#include <kernel/tty/fb_tty.h>
#include <drivers/serial/uart.h>
#include <kernel/board.h>
#include <kernel/net/socket.h>
#include <kernel/net/udp.h>
#include <kernel/net/icmp.h>
#include <kernel/net/ip.h>
#include <kernel/net/tcp.h>
#include <kernel/net/net.h>
#include <kernel/config.h>

#define AT_FDCWD     (-100)

/* mmap flags */
#define MAP_SHARED    0x01
#define MAP_PRIVATE   0x02
#define MAP_FIXED     0x10
#define MAP_ANONYMOUS 0x20

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

static void vma_add(process_t *p, uint64_t vaddr, uint64_t size, uint32_t flags)
{
	vma_t *v = kmalloc(sizeof(*v));
	if (!v) return;
	v->vaddr = vaddr;
	v->size  = size;
	v->flags = flags;
	v->next  = p->vmas;
	p->vmas  = v;
}

static void vma_free_all(process_t *p)
{
	vma_t *v = p->vmas;
	while (v) {
		vma_t *next = v->next;
		kfree(v);
		v = next;
	}
	p->vmas = NULL;
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
	extern volatile uint64_t kernel_ticks;
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
static long sys_mmap(uint64_t addr, size_t len, int prot, int flags, int fd, uint64_t off);
static long sys_munmap(uint64_t addr, size_t len);
static long sys_mprotect(uint64_t addr, size_t len, int prot);
static long sys_madvise(uint64_t addr, size_t len, int advice);
static long sys_msync(uint64_t addr, size_t len, int flags);

/* signal-management syscalls (defined in sys/syscall_sig.c) */
static long sys_rt_sigaction(int sig, const sigaction_t *act, sigaction_t *oldact, size_t sigsetsize);
static long sys_rt_sigprocmask(int how, const uint64_t *set, uint64_t *oldset, size_t sigsetsize);
static long sys_kill(pid_t pid, int sig);
static long sys_tgkill(pid_t tgid, pid_t tid, int sig);

#include "sys/syscall_fs.c"
#include "sys/syscall_proc.c"
#include "sys/syscall_io.c"
#include "sys/syscall_event.c"
#include "sys/syscall_net.c"
#include "sys/syscall_misc.c"
#include "sys/syscall_ptrace.c"
#include "sys/syscall_ipc.c"
#include "sys/syscall_mm.c"
#include "sys/syscall_sig.c"

void syscall_handle(arch_context_t *ctx)
{
	arch_get_cpu_ctx()->wd_where = "syscall";
	long n    = (long)CTX_SYSCALL_N(ctx);
	long arg0 = (long)CTX_SYSCALL_ARG0(ctx);
	long arg1 = (long)CTX_SYSCALL_ARG1(ctx);
	long arg2 = (long)CTX_SYSCALL_ARG2(ctx);
	long arg3 = (long)CTX_SYSCALL_ARG3(ctx);
	long arg4 = (long)CTX_SYSCALL_ARG4(ctx);
	long arg5 = (long)CTX_SYSCALL_ARG5(ctx);
	long ret  = -38;
	(void)arg5;

	task_current()->ctx = ctx;
	ptrace_stop_check(ctx, 0);


	switch (n) {
	/* ── I/O ─────────────────────────────────────────────── */
	case SYS_ARCH_READ:     ret = sys_read((int)arg0, (void*)arg1, (size_t)arg2); break;
	case SYS_ARCH_WRITE:    ret = sys_write((int)arg0, (void*)arg1, (size_t)arg2); break;
	case SYS_ARCH_SENDFILE: ret = sys_sendfile((int)arg0, (int)arg1, (long*)arg2, (size_t)arg3); break;
	case SYS_ARCH_READV:   ret = sys_readv((int)arg0, (const iovec_t*)arg1, (int)arg2); break;
	case SYS_ARCH_WRITEV:  ret = sys_writev((int)arg0, (const iovec_t*)arg1, (int)arg2); break;
	case SYS_ARCH_CLOSE:   ret = sys_close((int)arg0); break;
	case SYS_ARCH_LSEEK:   ret = sys_lseek((int)arg0, (off_t)arg1, (int)arg2); break;
	case SYS_ARCH_IOCTL:   ret = sys_ioctl((int)arg0, (uint32_t)arg1, (uint64_t)arg2); break;
	case SYS_ARCH_DUP:     ret = sys_dup((int)arg0); break;
	case SYS_ARCH_DUP3:    ret = sys_dup3((int)arg0, (int)arg1, (int)arg2); break;
	case SYS_ARCH_FCNTL:   ret = sys_fcntl((int)arg0, (int)arg1, (uint64_t)arg2); break;
	case SYS_ARCH_PIPE2:   ret = sys_pipe2((int*)arg0, (int)arg1); break;
#if defined(__x86_64__)
	case SYS_ARCH_PIPE:    ret = sys_pipe2((int*)arg0, 0); break;
#endif
	case SYS_ARCH_EPOLL_CREATE1: ret = sys_epoll_create1((int)arg0); break;
	case SYS_ARCH_EPOLL_CTL:     ret = sys_epoll_ctl((int)arg0, (int)arg1, (int)arg2, (linux_epoll_event_t*)arg3); break;
	case SYS_ARCH_EPOLL_PWAIT:   ret = sys_epoll_pwait((int)arg0, (linux_epoll_event_t*)arg1, (int)arg2, (int)arg3, (void*)arg4); break;
	/* ── open / stat ─────────────────────────────────────── */
#if defined(__x86_64__)
	case SYS_ARCH_OPEN:       ret = sys_open((const char*)arg0, (int)arg1, (mode_t)arg2); break;
#endif
	case SYS_ARCH_OPENAT:     ret = sys_openat((int)arg0, (const char*)arg1, (int)arg2, (mode_t)arg3); break;
	case SYS_ARCH_FSTAT:      ret = sys_fstat((int)arg0, (linux_stat_t*)arg1); break;
	case SYS_ARCH_NEWFSTATAT: ret = sys_newfstatat((int)arg0, (const char*)arg1, (linux_stat_t*)arg2, (int)arg3); break;
	case SYS_ARCH_GETDENTS64: ret = sys_getdents64((int)arg0, (void*)arg1, (size_t)arg2); break;
	/* ── fs ops ──────────────────────────────────────────── */
	case SYS_ARCH_CHDIR:    ret = sys_chdir((const char*)arg0); break;
	case SYS_ARCH_GETCWD:   ret = sys_getcwd((char*)arg0, (size_t)arg1); break;
	case SYS_ARCH_MKDIRAT:  ret = sys_mkdirat((int)arg0, (const char*)arg1, (mode_t)arg2); break;
	case SYS_ARCH_UNLINKAT: ret = sys_unlinkat((int)arg0, (const char*)arg1, (int)arg2); break;
	case SYS_ARCH_RENAMEAT: ret = sys_renameat((int)arg0, (const char*)arg1, (int)arg2, (const char*)arg3); break;
	case SYS_ARCH_FTRUNCATE: ret = sys_ftruncate((int)arg0, (off_t)arg1); break;
	case SYS_ARCH_FCHMOD:   ret = sys_fchmod((int)arg0, (mode_t)arg1); break;
	case SYS_ARCH_FCHMODAT: ret = sys_fchmodat((int)arg0, (const char*)arg1, (mode_t)arg2, (int)arg3); break;
	case SYS_ARCH_FCHOWN:   ret = sys_fchown((int)arg0, (uid_t)arg1, (gid_t)arg2); break;
	case SYS_ARCH_FCHOWNAT: ret = sys_fchownat((int)arg0, (const char*)arg1, (uid_t)arg2, (gid_t)arg3, 0); break;
	case SYS_ARCH_READLINKAT: ret = sys_readlinkat_vfs((int)arg0, (const char*)arg1, (char*)arg2, (size_t)arg3); break;
	case SYS_ARCH_SYMLINKAT: ret = sys_symlinkat((const char*)arg0, (int)arg1, (const char*)arg2); break;
	case SYS_ARCH_LINKAT:   ret = sys_linkat((int)arg0, (const char*)arg1, (int)arg2, (const char*)arg3, 0); break;
	case SYS_ARCH_FACCESSAT:  ret = sys_faccessat((int)arg0, (const char*)arg1, (int)arg2, (int)arg3); break;
	case SYS_ARCH_FACCESSAT2: ret = sys_faccessat((int)arg0, (const char*)arg1, (int)arg2, 0); break;
	/* ── poll / select ───────────────────────────────────── */
	case SYS_ARCH_PSELECT6: ret = sys_pselect6((int)arg0, (uint64_t*)arg1, (uint64_t*)arg2, (uint64_t*)arg3, (linux_timespec_t*)arg4, NULL); break;
	case SYS_ARCH_PPOLL:    ret = sys_ppoll((linux_pollfd_t*)arg0, (uint64_t)arg1, (linux_timespec_t*)arg2, NULL); break;
	/* ── memory ──────────────────────────────────────────── */
	case SYS_ARCH_BRK:     ret = sys_brk((uint64_t)arg0); break;
	case SYS_ARCH_MMAP:
		ret = sys_mmap((uint64_t)arg0, (size_t)arg1, (int)arg2, (int)arg3, (int)arg4, (uint64_t)arg5);
		break;
	case SYS_ARCH_MUNMAP:  ret = sys_munmap((uint64_t)arg0, (size_t)arg1); break;
	case SYS_ARCH_MPROTECT: ret = sys_mprotect((uint64_t)arg0, (size_t)arg1, (int)arg2); break;
	case SYS_ARCH_MADVISE: ret = sys_madvise((uint64_t)arg0, (size_t)arg1, (int)arg2); break;
	case SYS_ARCH_MSYNC:   ret = sys_msync((uint64_t)arg0, (size_t)arg1, (int)arg2); break;
	case SYS_ARCH_PRLIMIT64: ret = sys_prlimit64((pid_t)arg0, (int)arg1, (void*)arg2, (void*)arg3); break;
	/* ── process ─────────────────────────────────────────── */
	case SYS_ARCH_EXIT:        ret = sys_exit((int)arg0); break;
	case SYS_ARCH_EXIT_GROUP:  ret = sys_exit((int)arg0); break;
	case SYS_ARCH_REBOOT:      ret = sys_reboot((int)arg0, (int)arg1, (int)arg2, (void*)arg3); break;
	case SYS_ARCH_SYSLOG:      ret = sys_syslog((int)arg0, (char*)arg1, (int)arg2); break;
	case SYS_ARCH_UTIMENSAT:   ret = sys_utimensat((int)arg0, (const char*)arg1, (const void*)arg2, (int)arg3); break;
	case SYS_ARCH_GETPID:      ret = sys_getpid(); break;
	case SYS_ARCH_GETPPID:     ret = sys_getppid(); break;
	case SYS_ARCH_GETUID:      ret = sys_getuid(); break;
	case SYS_ARCH_GETEUID:     ret = sys_geteuid(); break;
	case SYS_ARCH_GETGID:      ret = sys_getgid(); break;
	case SYS_ARCH_GETEGID:     ret = sys_getegid(); break;
	case SYS_ARCH_GETTID:      ret = sys_gettid(); break;
	case SYS_ARCH_GETGROUPS:   ret = sys_getgroups((int)arg0, (uint64_t*)arg1); break;
	case SYS_ARCH_SETGROUPS:   ret = sys_setgroups((int)arg0, (const gid_t*)arg1); break;
	case SYS_ARCH_GETRLIMIT:   ret = sys_getrlimit((int)arg0, (void*)arg1); break;
	case SYS_ARCH_SETRLIMIT:   ret = sys_setrlimit((int)arg0, (const void*)arg1); break;
	case SYS_ARCH_SETUID:      ret = sys_setuid((uid_t)arg0); break;
	case SYS_ARCH_SETGID:      ret = sys_setgid((gid_t)arg0); break;
	case SYS_ARCH_SETPGID:     ret = sys_setpgid((pid_t)arg0, (pid_t)arg1); break;
	case SYS_ARCH_GETPGID:     ret = sys_getpgid((pid_t)arg0); break;
	case SYS_ARCH_SETSID:      ret = sys_setsid(); break;
	case SYS_ARCH_GETSID:      ret = sys_getsid((pid_t)arg0); break;
	case SYS_ARCH_GETPRIORITY: ret = sys_getpriority((int)arg0, (int)arg1); break;
	case SYS_ARCH_SETPRIORITY: ret = sys_setpriority((int)arg0, (int)arg1, (int)arg2); break;
	case SYS_ARCH_SIGALTSTACK: ret = sys_sigaltstack((const stack_t *)arg0, (stack_t *)arg1); break;
	case SYS_ARCH_UMASK:       ret = sys_umask((mode_t)arg0); break;
	case SYS_ARCH_SET_TID_ADDR:
		/* ABI: record the clear-on-exit address and return the tid. On
		 * thread exit the kernel writes 0 there and futex-wakes it. */
		task_current()->clear_child_tid = (uint32_t *)arg0;
		ret = (long)task_current()->pid;
		break;
	case SYS_ARCH_KILL:        ret = sys_kill((pid_t)arg0, (int)arg1); break;
	case SYS_ARCH_TGKILL:     ret = sys_tgkill((pid_t)arg0, (pid_t)arg1, (int)arg2); break;
	case SYS_ARCH_FUTEX:      ret = sys_futex((uint32_t*)arg0, (int)arg1, (uint32_t)arg2, (linux_timespec_t*)arg3, (uint32_t*)arg4, (uint32_t)arg5); break;
#if defined(__x86_64__)
	case SYS_ARCH_GETPGRP:    ret = (long)task_current()->pid; break;
#endif
	case SYS_ARCH_SCHED_YIELD: ret = sys_sched_yield(); break;
	case SYS_ARCH_SCHED_SETAFFINITY: ret = sys_sched_setaffinity((pid_t)arg0, (size_t)arg1, (void*)arg2); break;
	case SYS_ARCH_SCHED_GETAFFINITY: ret = sys_sched_getaffinity((pid_t)arg0, (size_t)arg1, (void*)arg2); break;
	case SYS_ARCH_PRCTL:      ret = sys_prctl((int)arg0, (uint64_t)arg1, (uint64_t)arg2, (uint64_t)arg3, (uint64_t)arg4); break;
	case SYS_ARCH_GETRANDOM:  ret = sys_getrandom((void*)arg0, (size_t)arg1, (unsigned int)arg2); break;
	case SYS_ARCH_STATX:      ret = sys_statx((int)arg0, (const char*)arg1, (int)arg2, (unsigned int)arg3, (linux_statx_t*)arg4); break;
	case SYS_ARCH_MEMBARRIER: ret = 0; break;
	case SYS_ARCH_RSEQ:       ret = -22; break;
	case SYS_ARCH_SET_ROBUST_LIST: ret = 0; break;
	case SYS_ARCH_GET_ROBUST_LIST: ret = -38; break;
	case SYS_ARCH_RT_SIGACTION: ret = sys_rt_sigaction((int)arg0, (const sigaction_t*)arg1, (sigaction_t*)arg2, (size_t)arg3); break;
	case SYS_ARCH_RT_SIGPROCMASK: ret = sys_rt_sigprocmask((int)arg0, (const uint64_t*)arg1, (uint64_t*)arg2, (size_t)arg3); break;
	case SYS_ARCH_RT_SIGRETURN: sys_rt_sigreturn(ctx); return; /* returns directly, no ret value */
	case SYS_ARCH_VFORK:  ret = sys_vfork(); break;
#ifdef SYS_ARCH_FORK
	case SYS_ARCH_FORK:   ret = sys_fork(); break;
#endif
	case SYS_ARCH_CLONE: {
		uint64_t flags = (uint64_t)arg0;
#define CLONE_VM       0x00000100
#define CLONE_THREAD   0x00010000
#define CLONE_SETTLS   0x00080000
#define CLONE_PTID     0x00100000 /* CLONE_PARENT_SETTID */
#define CLONE_CTID     0x00200000 /* CLONE_CHILD_CLEARTID */
#define CLONE_CSETTID  0x01000000 /* CLONE_CHILD_SETTID */
#define CLONE_VFORK    0x00004000
		if (flags & CLONE_VFORK) {
			ret = sys_vfork();
		} else if (flags & CLONE_VM) {
			/* Thread: share address space */
			uint64_t newsp  = (uint64_t)arg1;
			/* ABI: x86_64 clone(flags,newsp,ptid,ctid,tls)
			 *            aarch64 clone(flags,newsp,ptid,tls,ctid) */
			uint32_t *ptid  = (flags & CLONE_PTID)   ? (uint32_t*)arg2 : NULL;
#if defined(__aarch64__)
			uint64_t tls    = (flags & CLONE_SETTLS) ? (uint64_t)arg3 : 0;
			uint32_t *ctid_arg = (uint32_t*)arg4;
#else
			uint32_t *ctid_arg = (uint32_t*)arg3;
			uint64_t tls    = (flags & CLONE_SETTLS) ? (uint64_t)arg4 : 0;
#endif
			/* CLEARTID: clear+wake on exit (don't touch at creation).
			 * SETTID: write the tid into *ctid at creation. These are
			 * distinct flags sharing the one ctid argument. */
			uint32_t *clear_tid = (flags & CLONE_CTID)    ? ctid_arg : NULL;
			uint32_t *set_tid   = (flags & CLONE_CSETTID) ? ctid_arg : NULL;
			ret = (long)proc_clone_thread(newsp, tls, clear_tid, ptid, set_tid);
		} else {
			ret = sys_fork();
		}
		break;
	}
	case SYS_ARCH_UNSHARE: ret = sys_unshare((unsigned long)arg0); break;
	case SYS_ARCH_EXECVE: ret = sys_execve((const char*)arg0, (char * const *)arg1, (char * const *)arg2); break;
	case SYS_ARCH_WAIT4:  ret = sys_waitpid((pid_t)arg0, (int*)arg1, (int)arg2); break;
	/* ── time ────────────────────────────────────────────── */
	case SYS_ARCH_CLOCK_GETTIME: ret = sys_clock_gettime((int)arg0, (linux_timespec_t*)arg1); break;
	case SYS_ARCH_GETTIMEOFDAY:  ret = sys_gettimeofday((linux_timeval_t*)arg0, (void*)arg1); break;
	case SYS_ARCH_NANOSLEEP:     ret = sys_nanosleep((linux_timespec_t*)arg0, (linux_timespec_t*)arg1); break;
	case SYS_ARCH_GETITIMER:     ret = sys_getitimer((int)arg0, (itimerval_t*)arg1); break;
	case SYS_ARCH_SETITIMER:     ret = sys_setitimer((int)arg0, (const itimerval_t*)arg1, (itimerval_t*)arg2); break;
	case SYS_ARCH_TIMES:         ret = sys_times((void*)arg0); break;
	/* ── misc ────────────────────────────────────────────── */
	case SYS_ARCH_UNAME:   ret = sys_uname((linux_utsname_t*)arg0); break;
	case SYS_ARCH_SYSINFO: ret = sys_sysinfo((void*)arg0); break;
#if defined(__x86_64__)
	case SYS_ARCH_STAT:    ret = sys_stat_path((const char*)arg0, (linux_stat_t*)arg1); break;
	case SYS_ARCH_LSTAT:   ret = sys_stat_path((const char*)arg0, (linux_stat_t*)arg1); break;
	case SYS_ARCH_ACCESS:  ret = sys_access((const char*)arg0, (int)arg1); break;
	case SYS_ARCH_SELECT:  ret = sys_select((int)arg0, (uint64_t*)arg1, (uint64_t*)arg2, (uint64_t*)arg3, (linux_timeval_t*)arg4); break;
	case SYS_ARCH_POLL:    ret = sys_poll((linux_pollfd_t*)arg0, (uint64_t)arg1, (int)arg2); break;
	case SYS_ARCH_DUP2:    ret = sys_dup2((int)arg0, (int)arg1); break;
	case SYS_ARCH_MKDIR:   ret = sys_mkdir((const char*)arg0, (mode_t)arg1); break;
	case SYS_ARCH_RMDIR:   ret = sys_rmdir((const char*)arg0); break;
	case SYS_ARCH_UNLINK:  ret = sys_unlink((const char*)arg0); break;
	case SYS_ARCH_LINK:    ret = sys_linkat(AT_FDCWD, (const char*)arg0, AT_FDCWD, (const char*)arg1, 0); break;
	case SYS_ARCH_SYMLINK: ret = sys_symlink((const char*)arg0, (const char*)arg1); break;
	case SYS_ARCH_READLINK: ret = sys_readlinkat_vfs(AT_FDCWD, (const char*)arg0, (char*)arg1, (size_t)arg2); break;
	case SYS_ARCH_RENAME:  ret = sys_rename((const char*)arg0, (const char*)arg1); break;
	case SYS_ARCH_CHMOD:   ret = sys_chmod((const char*)arg0, (mode_t)arg1); break;
	case SYS_ARCH_CHOWN:   ret = sys_chown((const char*)arg0, (uid_t)arg1, (gid_t)arg2); break;
	case SYS_ARCH_TRUNCATE: ret = sys_truncate((const char*)arg0, (off_t)arg1); break;
	case SYS_ARCH_ARCH_PRCTL: ret = sys_arch_prctl((int)arg0, (uint64_t)arg1); break;
	case SYS_ARCH_EPOLL_CREATE: ret = sys_epoll_create1(0); break;
	case SYS_ARCH_EPOLL_WAIT:   ret = sys_epoll_pwait((int)arg0, (linux_epoll_event_t*)arg1, (int)arg2, (int)arg3, NULL); break;
#endif
	/* ── sockets ─────────────────────────────────────────── */
	case SYS_ARCH_SOCKET:      ret = sys_socket((int)arg0, (int)arg1, (int)arg2); break;
	case SYS_ARCH_BIND:        ret = sys_bind((int)arg0, (const void*)arg1, (uint32_t)arg2); break;
	case SYS_ARCH_CONNECT:     ret = sys_connect((int)arg0, (const void*)arg1, (uint32_t)arg2); break;
	case SYS_ARCH_LISTEN:      ret = sys_listen((int)arg0, (int)arg1); break;
	case SYS_ARCH_ACCEPT:      ret = sys_accept4((int)arg0, (void*)arg1, (uint32_t*)arg2, 0); break;
	case SYS_ARCH_ACCEPT4:     ret = sys_accept4((int)arg0, (void*)arg1, (uint32_t*)arg2, (int)arg3); break;
	case SYS_ARCH_SENDTO:      ret = sys_sendto((int)arg0, (void*)arg1, (size_t)arg2, (int)arg3, (void*)arg4, (uint32_t)arg5); break;
	case SYS_ARCH_RECVFROM:    ret = sys_recvfrom((int)arg0, (void*)arg1, (size_t)arg2, (int)arg3, (void*)arg4, (uint32_t*)arg5); break;
	case SYS_ARCH_GETSOCKNAME: ret = sys_getsockname((int)arg0, (void*)arg1, (uint32_t*)arg2); break;
	case SYS_ARCH_GETPEERNAME: ret = sys_getpeername((int)arg0, (void*)arg1, (uint32_t*)arg2); break;
	case SYS_ARCH_SETSOCKOPT:  ret = sys_setsockopt((int)arg0, (int)arg1, (int)arg2, (void*)arg3, (uint32_t)arg4); break;
	case SYS_ARCH_GETSOCKOPT:  ret = sys_getsockopt((int)arg0, (int)arg1, (int)arg2, (void*)arg3, (uint32_t*)arg4); break;
	case SYS_ARCH_SHUTDOWN:    ret = sys_shutdown((int)arg0, (int)arg1); break;
	/* ── missing batch ───────────────────────────────────────── */
	case SYS_ARCH_PREAD64:     ret = sys_pread64((int)arg0, (void*)arg1, (size_t)arg2, (off_t)arg3); break;
	case SYS_ARCH_PWRITE64:    ret = sys_pwrite64((int)arg0, (void*)arg1, (size_t)arg2, (off_t)arg3); break;
	case SYS_ARCH_MREMAP:      ret = sys_mremap((uint64_t)arg0, (size_t)arg1, (size_t)arg2, (int)arg3, (uint64_t)arg4); break;
	case SYS_ARCH_FLOCK:       ret = sys_flock((int)arg0, (int)arg1); break;
	case SYS_ARCH_FSYNC:       ret = sys_fsync((int)arg0); break;
	case SYS_ARCH_FDATASYNC:   ret = sys_fdatasync((int)arg0); break;
	case SYS_ARCH_FCHDIR:      ret = sys_fchdir((int)arg0); break;
	case SYS_ARCH_GETRUSAGE:   ret = sys_getrusage((int)arg0, (void*)arg1); break;
	case SYS_ARCH_SETREUID:    ret = sys_setreuid((uid_t)arg0, (uid_t)arg1); break;
	case SYS_ARCH_SETREGID:    ret = sys_setregid((gid_t)arg0, (gid_t)arg1); break;
	case SYS_ARCH_SETRESUID:   ret = sys_setresuid((uid_t)arg0, (uid_t)arg1, (uid_t)arg2); break;
	case SYS_ARCH_GETRESUID:   ret = sys_getresuid((uid_t*)arg0, (uid_t*)arg1, (uid_t*)arg2); break;
	case SYS_ARCH_SETRESGID:   ret = sys_setresgid((gid_t)arg0, (gid_t)arg1, (gid_t)arg2); break;
	case SYS_ARCH_GETRESGID:   ret = sys_getresgid((gid_t*)arg0, (gid_t*)arg1, (gid_t*)arg2); break;
	case SYS_ARCH_RT_SIGPENDING: ret = sys_rt_sigpending((uint64_t*)arg0, (size_t)arg1); break;
	case SYS_ARCH_RT_SIGSUSPEND: ret = sys_rt_sigsuspend((const uint64_t*)arg0, (size_t)arg1); break;
	case SYS_ARCH_MLOCK:       ret = sys_mlock((uint64_t)arg0, (size_t)arg1); break;
	case SYS_ARCH_MUNLOCK:     ret = sys_munlock((uint64_t)arg0, (size_t)arg1); break;
	case SYS_ARCH_MLOCK2:      ret = sys_mlock2((uint64_t)arg0, (size_t)arg1, (int)arg2); break;
	case SYS_ARCH_SYNC:        ret = sys_sync(); break;
	case SYS_ARCH_MOUNT:       ret = sys_mount((const char*)arg0, (const char*)arg1, (const char*)arg2, (unsigned long)arg3, (const void*)arg4); break;
	case SYS_ARCH_UMOUNT2:     ret = sys_umount2((const char*)arg0, (int)arg1); break;
	case SYS_ARCH_SETHOSTNAME: ret = sys_sethostname((const char*)arg0, (size_t)arg1); break;
	case SYS_ARCH_TKILL:       ret = sys_tkill((pid_t)arg0, (int)arg1); break;
	case SYS_ARCH_CLOCK_NANOSLEEP: ret = sys_clock_nanosleep((int)arg0, (int)arg1, (linux_timespec_t*)arg2, (linux_timespec_t*)arg3); break;
	case SYS_ARCH_WAITID:      ret = sys_waitid((int)arg0, (int)arg1, (void*)arg2, (int)arg3, (void*)arg4); break;
	case SYS_ARCH_SPLICE:      ret = sys_splice((int)arg0, (off_t*)arg1, (int)arg2, (off_t*)arg3, (size_t)arg4, (unsigned int)arg5); break;
	case SYS_ARCH_FALLOCATE:   ret = sys_fallocate((int)arg0, (int)arg1, (off_t)arg2, (off_t)arg3); break;
	case SYS_ARCH_COPY_FILE_RANGE: ret = sys_copy_file_range((int)arg0, (off_t*)arg1, (int)arg2, (off_t*)arg3, (size_t)arg4, (unsigned int)arg5); break;
	case SYS_ARCH_SOCKETPAIR:  ret = sys_socketpair((int)arg0, (int)arg1, (int)arg2, (int*)arg3); break;
	/* ── fd-based notifications ──────────────────────────────── */
#if defined(__x86_64__)
	case SYS_ARCH_EVENTFD:
#endif
	case SYS_ARCH_EVENTFD2:    ret = sys_eventfd2((unsigned int)arg0, (int)arg1); break;
	case SYS_ARCH_TIMERFD_CREATE:  ret = sys_timerfd_create((int)arg0, (int)arg1); break;
	case SYS_ARCH_TIMERFD_SETTIME: ret = sys_timerfd_settime((int)arg0, (int)arg1, (linux_itimerspec_t*)arg2, (linux_itimerspec_t*)arg3); break;
	case SYS_ARCH_TIMERFD_GETTIME: ret = sys_timerfd_gettime((int)arg0, (linux_itimerspec_t*)arg1); break;
#if defined(__x86_64__)
	case SYS_ARCH_INOTIFY_INIT:
#endif
	case SYS_ARCH_INOTIFY_INIT1:      ret = sys_inotify_init1((int)arg0); break;
	case SYS_ARCH_INOTIFY_ADD_WATCH:  ret = sys_inotify_add_watch((int)arg0, (const char*)arg1, (uint32_t)arg2); break;
	case SYS_ARCH_INOTIFY_RM_WATCH:   ret = sys_inotify_rm_watch((int)arg0, (int)arg1); break;
	case SYS_ARCH_SIGNALFD4:   ret = sys_signalfd4((int)arg0, (const uint64_t*)arg1, (size_t)arg2, (int)arg3); break;
	case SYS_ARCH_MEMFD_CREATE: ret = sys_memfd_create((const char*)arg0, (unsigned int)arg1); break;
	/* ── arch-specific ───────────────────────────────────────── */
#if defined(__x86_64__)
	case SYS_ARCH_PAUSE:       ret = sys_pause(); break;
	case SYS_ARCH_ALARM:       ret = sys_alarm((unsigned int)arg0); break;
	case SYS_ARCH_CREAT:       ret = sys_creat((const char*)arg0, (mode_t)arg1); break;
	case SYS_ARCH_LCHOWN:      ret = sys_lchown((const char*)arg0, (uid_t)arg1, (gid_t)arg2); break;
	case SYS_ARCH_MKNOD:       ret = sys_mknod((const char*)arg0, (mode_t)arg1, (uint64_t)arg2); break;
	case SYS_ARCH_TIME_SYSCALL: ret = sys_time_syscall((long*)arg0); break;
	case SYS_ARCH_RENAMEAT2:   ret = sys_renameat2((int)arg0, (const char*)arg1, (int)arg2, (const char*)arg3, (unsigned int)arg4); break;
#else
	case SYS_ARCH_MKNODAT:     ret = sys_mknodat((int)arg0, (const char*)arg1, (mode_t)arg2, (uint64_t)arg3); break;
	case SYS_ARCH_RENAMEAT2:   ret = sys_renameat2((int)arg0, (const char*)arg1, (int)arg2, (const char*)arg3, (unsigned int)arg4); break;
#endif
	case SYS_ARCH_STATFS:    ret = sys_statfs((const char*)arg0, (kernel_statfs_t*)arg1); break;
	case SYS_ARCH_FSTATFS:   ret = sys_fstatfs((int)arg0, (kernel_statfs_t*)arg1); break;
	case SYS_ARCH_CAPGET:    ret = sys_capget((void*)arg0, (void*)arg1); break;
	case SYS_ARCH_CAPSET:    ret = sys_capset((void*)arg0, (const void*)arg1); break;
	case SYS_ARCH_GETXATTR:  ret = sys_getxattr((const char*)arg0,(const char*)arg1,(void*)arg2,(size_t)arg3); break;
	case SYS_ARCH_LGETXATTR: ret = sys_lgetxattr((const char*)arg0,(const char*)arg1,(void*)arg2,(size_t)arg3); break;
	case SYS_ARCH_FGETXATTR: ret = sys_fgetxattr((int)arg0,(const char*)arg1,(void*)arg2,(size_t)arg3); break;
	case SYS_ARCH_SETXATTR:  ret = sys_setxattr((const char*)arg0,(const char*)arg1,(const void*)arg2,(size_t)arg3,(int)arg4); break;
	case SYS_ARCH_LSETXATTR: ret = sys_lsetxattr((const char*)arg0,(const char*)arg1,(const void*)arg2,(size_t)arg3,(int)arg4); break;
	case SYS_ARCH_LISTXATTR: ret = sys_listxattr((const char*)arg0,(char*)arg1,(size_t)arg2); break;
	case SYS_ARCH_LLISTXATTR:ret = sys_llistxattr((const char*)arg0,(char*)arg1,(size_t)arg2); break;
	case SYS_ARCH_REMOVEXATTR:ret = sys_removexattr((const char*)arg0,(const char*)arg1); break;
	case SYS_ARCH_PTRACE:    ret = sys_ptrace((long)arg0, (pid_t)arg1, (uint64_t)arg2, (uint64_t)arg3); break;
	/* SysV IPC */
	case SYS_ARCH_SHMGET:   ret = sys_shmget((key_t)arg0, (size_t)arg1, (int)arg2); break;
	case SYS_ARCH_SHMAT:    ret = sys_shmat((int)arg0, (const void*)arg1, (int)arg2); break;
	case SYS_ARCH_SHMDT:    ret = sys_shmdt((const void*)arg0); break;
	case SYS_ARCH_SHMCTL:   ret = sys_shmctl((int)arg0, (int)arg1, (void*)arg2); break;
	case SYS_ARCH_SEMGET:   ret = sys_semget((key_t)arg0, (int)arg1, (int)arg2); break;
	case SYS_ARCH_SEMOP:    ret = sys_semop((int)arg0, (const sembuf_t*)arg1, (size_t)arg2); break;
	case SYS_ARCH_SEMCTL:   ret = sys_semctl((int)arg0, (int)arg1, (int)arg2, (uint64_t)arg3); break;
	/* POSIX IPC */
	case SYS_ARCH_MQ_OPEN:         ret = sys_mq_open((const char*)arg0,(int)arg1,(mode_t)arg2,(mq_attr_t*)arg3); break;
	case SYS_ARCH_MQ_UNLINK:       ret = sys_mq_unlink((const char*)arg0); break;
	case SYS_ARCH_MQ_TIMEDSEND:    ret = sys_mq_timedsend((int)arg0,(const char*)arg1,(size_t)arg2,(uint32_t)arg3,(const void*)arg4); break;
	case SYS_ARCH_MQ_TIMEDRECEIVE: ret = sys_mq_timedreceive((int)arg0,(char*)arg1,(size_t)arg2,(uint32_t*)arg3,(const void*)arg4); break;
	case SYS_ARCH_MQ_GETSETATTR:   ret = sys_mq_getsetattr((int)arg0,(const mq_attr_t*)arg1,(mq_attr_t*)arg2); break;
	default:
		kprintf("syscall: unimplemented %ld (pid=%u)\n",
		        n, task_current()->pid);
		ret = -38; /* ENOSYS */
		break;
	}

	CTX_SYSCALL_RET(ctx, ret);
	task_current()->ctx = ctx;
	ptrace_stop_check(ctx, 1);
}
