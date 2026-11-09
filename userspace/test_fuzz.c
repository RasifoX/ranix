/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* Unprivileged syscall fuzzer. Random arguments against the x86_64 syscall table,
 * drawn from pools of live fds, mapped and unmapped buffers, kernel addresses,
 * boundary integers and paths.
 *
 *   test_fuzz SEED BATCHES [THREADS] [ONLY_BATCH]
 *
 * Each batch is a forked child running CALLS_PER_BATCH calls from its own stream,
 * derived from (SEED, batch), so a batch replays alone: FUZZ_VERBOSE=1 with
 * ONLY_BATCH prints every call to stderr before making it. THREADS=1 adds a second
 * thread on another stream to reach the close-versus-use races; that mode is not
 * reproducible call for call. FUZZ_RANGE=lo-hi narrows a crash to syscall numbers.
 *
 * A child dying on a signal is expected, its memory being fuzzed too. What the
 * parent reports is a child that will not die: FUZZ-HANG. A kernel panic ends the
 * run on the console, and the last FUZZ-BATCH line names the batch. */
#define _GNU_SOURCE
#include <sched.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/timerfd.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <sys/prctl.h>

#define CALLS_PER_BATCH 2000
#define NFDS            24
#define WORKDIR         "/tmp/fz"
#define UID_FUZZ        1000
#define NSYSCALL        460

/* would end the batch at once, or reach outside it */
static const int excluded[] = {
	SYS_exit, SYS_exit_group, SYS_kill, SYS_tkill, SYS_tgkill,
	SYS_rt_sigqueueinfo, SYS_rt_tgsigqueueinfo, SYS_pidfd_send_signal,
	SYS_fork, SYS_vfork, SYS_clone, SYS_clone3,
	SYS_pause, SYS_rt_sigsuspend, SYS_rt_sigtimedwait,   /* would swallow the alarm */
};

static int fds[NFDS];
static char *buf_rw, *buf_ro, *buf_none, *buf_edge;

typedef struct { uint64_t s; } rng_t;

static uint64_t rnd(rng_t *r)
{
	uint64_t z = (r->s += 0x9e3779b97f4a7c15ULL);
	z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
	z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
	return z ^ (z >> 31);
}

static uint64_t pick(rng_t *r, const uint64_t *v, int n) { return v[rnd(r) % (uint64_t)n]; }

/* nothing executable: a successful exec would end the batch */
static const char *const paths[] = {
	WORKDIR "/a", WORKDIR "/b", WORKDIR "/d", WORKDIR "/d/x", WORKDIR "/l", WORKDIR,
	"a", "d/x", ".", "..", "", "/", "/tmp", "/proc/self/maps", "/proc/self/mem",
	"/proc/self/status", "/proc/self/fd/3", "/proc/meminfo", "/proc/sys/kernel/hostname",
	"/sys", "/dev/null", "/dev/zero", "/dev/tty", "/dev/ptmx", "/dev/kmsg", "/etc/passwd",
	WORKDIR "/f", WORKDIR "/a/", WORKDIR "/nonexistent/x",
};

static char longpath[5000];

static uint64_t arg_path(rng_t *r)
{
	if (rnd(r) % 16 == 0) {
		size_t n = 200 + rnd(r) % (sizeof(longpath) - 201);
		memset(longpath, 'A' + (int)(rnd(r) % 26), n);
		longpath[0] = '/';
		longpath[n] = '\0';
		return (uint64_t)longpath;
	}
	return (uint64_t)paths[rnd(r) % (sizeof(paths) / sizeof(paths[0]))];
}

static uint64_t arg_ptr(rng_t *r)
{
	static const uint64_t bad[] = {
		0, 1, 8, 0xfff, 0x1000, 0x10000,
		0x7fffffffffULL, 0x7ffffffff8ULL, 0x8000000000ULL, 0x800000000000ULL,
		0x0000800000000000ULL, 0x7fffffffffffULL, 0x8000000000000000ULL,
		0xffff800000000000ULL, 0xffff800000100000ULL,
		0xffffffff80000000ULL, 0xffffffff80100000ULL,
		0xfffffffffffff000ULL, 0xffffffffffffffffULL,
	};
	switch (rnd(r) % 8) {
	case 0: case 1: case 2: case 3:
		return (uint64_t)buf_rw + (rnd(r) % 4096 & ~7ULL);
	case 4: return (uint64_t)buf_ro;
	case 5: return (uint64_t)buf_none + rnd(r) % 4096;
	case 6: return (uint64_t)buf_edge + 4096 - (rnd(r) % 32);   /* runs into a hole */
	default: return pick(r, bad, sizeof(bad) / sizeof(bad[0]));
	}
}

static uint64_t arg_int(rng_t *r)
{
	static const uint64_t edge[] = {
		0, 1, 2, 3, 4, 7, 8, 16, 32, 63, 64, 127, 128, 255, 256, 512, 1024,
		4095, 4096, 4097, 8192, 65535, 65536, 0x7fffffff, 0x80000000ULL, 0xffffffffULL,
		0x100000000ULL, 0x1000000000ULL, 0x7fffffffffffffffULL, 0x8000000000000000ULL,
		0xfffffffffffff000ULL, 0xffffffffffffffffULL, (uint64_t)-2, (uint64_t)-100,
	};
	switch (rnd(r) % 4) {
	case 0:  return rnd(r) & 0xff;
	case 1:  return rnd(r);
	default: return pick(r, edge, sizeof(edge) / sizeof(edge[0]));
	}
}

static uint64_t arg_fd(rng_t *r)
{
	if (rnd(r) % 8 == 0) return arg_int(r);
	return (uint64_t)(int64_t)fds[rnd(r) % NFDS];
}

static uint64_t arg_any(rng_t *r)
{
	switch (rnd(r) % 8) {
	case 0: case 1: return arg_fd(r);
	case 2: case 3: case 4: return arg_ptr(r);
	case 5: return arg_path(r);
	default: return arg_int(r);
	}
}

/* Argument shapes for the calls where a blind draw rarely gets past the first
 * check. 'f' fd, 'p' pointer, 's' path, 'i' integer, 'a' anything. */
typedef struct { int nr; const char *sig; } shape_t;
static const shape_t shapes[] = {
	{ SYS_read, "fpi" }, { SYS_write, "fpi" }, { SYS_readv, "fpi" }, { SYS_writev, "fpi" },
	{ SYS_pread64, "fpii" }, { SYS_pwrite64, "fpii" }, { SYS_lseek, "fii" },
	{ SYS_open, "sii" }, { SYS_openat, "fsii" }, { SYS_stat, "sp" }, { SYS_lstat, "sp" },
	{ SYS_fstat, "fp" }, { SYS_newfstatat, "fspi" }, { SYS_statx, "fsiip" }, { SYS_statfs, "sp" },
	{ SYS_fstatfs, "fp" }, { SYS_access, "si" }, { SYS_faccessat, "fsi" }, { SYS_chdir, "s" },
	{ SYS_fchdir, "f" }, { SYS_mkdir, "si" }, { SYS_mkdirat, "fsi" }, { SYS_rmdir, "s" },
	{ SYS_unlink, "s" }, { SYS_unlinkat, "fsi" }, { SYS_rename, "ss" }, { SYS_renameat, "fsfs" },
	{ SYS_link, "ss" }, { SYS_linkat, "fsfsi" }, { SYS_symlink, "ss" }, { SYS_symlinkat, "sfs" },
	{ SYS_readlink, "spi" }, { SYS_readlinkat, "fspi" }, { SYS_chmod, "si" }, { SYS_fchmod, "fi" },
	{ SYS_chown, "sii" }, { SYS_fchown, "fii" }, { SYS_truncate, "si" }, { SYS_ftruncate, "fi" },
	{ SYS_getdents64, "fpi" }, { SYS_fcntl, "fia" }, { SYS_ioctl, "fia" }, { SYS_dup, "f" },
	{ SYS_dup2, "fi" }, { SYS_dup3, "fii" }, { SYS_close, "f" }, { SYS_flock, "fi" },
	{ SYS_fsync, "f" }, { SYS_poll, "pii" }, { SYS_ppoll, "pipp" }, { SYS_select, "ipppp" },
	{ SYS_pselect6, "ippppp" }, { SYS_epoll_ctl, "fifp" }, { SYS_epoll_wait, "fpii" },
	{ SYS_epoll_pwait, "fpiip" }, { SYS_sendfile, "ffpi" }, { SYS_splice, "fpfpii" },
	{ SYS_copy_file_range, "fpfpii" }, { SYS_fallocate, "fiii" },
	{ SYS_mmap, "iiiifi" }, { SYS_mprotect, "pii" }, { SYS_madvise, "pii" }, { SYS_msync, "pii" },
	{ SYS_mremap, "piiip" }, { SYS_munmap, "pi" }, { SYS_mlock, "pi" }, { SYS_munlock, "pi" },
	{ SYS_socket, "iii" }, { SYS_bind, "fpi" }, { SYS_connect, "fpi" }, { SYS_listen, "fi" },
	{ SYS_accept4, "fppi" }, { SYS_sendto, "fpiipi" }, { SYS_recvfrom, "fpiipp" },
	{ SYS_sendmsg, "fpi" }, { SYS_recvmsg, "fpi" }, { SYS_sendmmsg, "fpii" }, { SYS_recvmmsg, "fpiip" },
	{ SYS_getsockname, "fpp" }, { SYS_getpeername, "fpp" }, { SYS_setsockopt, "fiipi" },
	{ SYS_getsockopt, "fiipp" }, { SYS_shutdown, "fi" }, { SYS_socketpair, "iiip" },
	{ SYS_execve, "spp" }, { SYS_execveat, "fsppi" }, { SYS_wait4, "ipip" }, { SYS_waitid, "iipip" },
	{ SYS_rt_sigaction, "ippi" }, { SYS_rt_sigprocmask, "ippi" }, { SYS_sigaltstack, "pp" },
	{ SYS_futex, "piippi" }, { SYS_set_robust_list, "pi" }, { SYS_get_robust_list, "ipp" },
	{ SYS_set_tid_address, "p" }, { SYS_arch_prctl, "ii" }, { SYS_prctl, "iiiii" },
	{ SYS_process_vm_readv, "ipipii" }, { SYS_process_vm_writev, "ipipii" }, { SYS_ptrace, "iipp" },
	{ SYS_getxattr, "sspi" }, { SYS_setxattr, "sspii" }, { SYS_listxattr, "spi" },
	{ SYS_removexattr, "ss" }, { SYS_fgetxattr, "fspi" }, { SYS_utimensat, "fspi" },
	{ SYS_inotify_add_watch, "fsi" }, { SYS_inotify_rm_watch, "fi" }, { SYS_signalfd4, "fpii" },
	{ SYS_timerfd_settime, "fipp" }, { SYS_timerfd_gettime, "fp" }, { SYS_memfd_create, "si" },
	{ SYS_mq_open, "siip" }, { SYS_mq_timedsend, "fpiip" }, { SYS_mq_timedreceive, "fpipp" },
	{ SYS_mq_getsetattr, "fpp" }, { SYS_shmat, "ipi" }, { SYS_shmdt, "p" }, { SYS_shmctl, "iip" },
	{ SYS_semop, "ipi" }, { SYS_semctl, "iiia" }, { SYS_msgsnd, "ipii" }, { SYS_msgrcv, "ipiii" },
	{ SYS_msgctl, "iip" }, { SYS_io_uring_setup, "ip" }, { SYS_io_uring_enter, "fiiipi" },
	{ SYS_io_uring_register, "fipi" }, { SYS_seccomp, "iip" }, { SYS_getrandom, "pii" },
	{ SYS_sched_setaffinity, "iip" }, { SYS_sched_getaffinity, "iip" }, { SYS_prlimit64, "iipp" },
	{ SYS_clock_gettime, "ip" }, { SYS_clock_nanosleep, "iipp" }, { SYS_nanosleep, "pp" },
	{ SYS_getitimer, "ip" }, { SYS_setitimer, "ipp" }, { SYS_adjtimex, "p" }, { SYS_uname, "p" },
	{ SYS_sysinfo, "p" }, { SYS_times, "p" }, { SYS_getcwd, "pi" }, { SYS_getgroups, "ip" },
	{ SYS_capget, "pp" }, { SYS_capset, "pp" }, { SYS_syslog, "ipi" }, { SYS_mount, "sssip" },
	{ SYS_umount2, "si" }, { SYS_chroot, "s" }, { SYS_pivot_root, "ss" }, { SYS_setns, "fi" },
	{ SYS_unshare, "i" }, { SYS_pidfd_open, "ii" }, { SYS_close_range, "iii" },
};

static uint64_t arg_for(rng_t *r, char k)
{
	switch (k) {
	case 'f': return arg_fd(r);
	case 'p': return arg_ptr(r);
	case 's': return arg_path(r);
	case 'i': return arg_int(r);
	default:  return arg_any(r);
	}
}

static int verbose;
static int range_lo = 0, range_hi = NSYSCALL;   /* FUZZ_RANGE=lo-hi: bisecting a crash */
static volatile uint64_t *calls_made;   /* shared with the parent, which counts */

static void one_call(rng_t *r)
{
	uint64_t a[6];
	int nr;
	if (rnd(r) % 8) {
		const shape_t *s = &shapes[rnd(r) % (sizeof(shapes) / sizeof(shapes[0]))];
		nr = s->nr;
		size_t n = strlen(s->sig);
		for (int i = 0; i < 6; i++)
			a[i] = (size_t)i < n ? arg_for(r, s->sig[i]) : arg_any(r);
	} else {
		for (;;) {
			nr = (int)(rnd(r) % NSYSCALL);
			int skip = 0;
			for (size_t i = 0; i < sizeof(excluded) / sizeof(excluded[0]); i++)
				skip |= excluded[i] == nr;
			if (!skip) break;
		}
		for (int i = 0; i < 6; i++) a[i] = arg_any(r);
	}
	for (size_t i = 0; i < sizeof(excluded) / sizeof(excluded[0]); i++)
		if (excluded[i] == nr) return;
	if (nr < range_lo || nr >= range_hi) return;
	if (verbose)
		dprintf(2, "call %d %#llx %#llx %#llx %#llx %#llx %#llx\n", nr,
		        (unsigned long long)a[0], (unsigned long long)a[1], (unsigned long long)a[2],
		        (unsigned long long)a[3], (unsigned long long)a[4], (unsigned long long)a[5]);
	(*calls_made)++;
	/* wait4 on an empty group, or read on a silent pipe, ends at the interval timer */
	syscall(nr, a[0], a[1], a[2], a[3], a[4], a[5]);
}

static void *runner(void *p)
{
	rng_t *r = p;
	for (int i = 0; i < CALLS_PER_BATCH; i++) one_call(r);
	return NULL;
}

static void on_alarm(int s) { (void)s; }

static void fill_pool(void)
{
	int sv[2], pf[2], n = 0;
	fds[n++] = open(WORKDIR "/a", O_RDWR | O_CREAT, 0644);
	fds[n++] = open(WORKDIR "/d", O_RDONLY | O_DIRECTORY);
	fds[n++] = open("/proc/self/maps", O_RDONLY);
	fds[n++] = open("/dev/null", O_RDWR);
	fds[n++] = open("/dev/zero", O_RDWR);
	fds[n++] = open("/dev/ptmx", O_RDWR);
	pipe(pf); fds[n++] = pf[0]; fds[n++] = pf[1];
	socketpair(AF_UNIX, SOCK_STREAM, 0, sv); fds[n++] = sv[0]; fds[n++] = sv[1];
	socketpair(AF_UNIX, SOCK_DGRAM, 0, sv); fds[n++] = sv[0]; fds[n++] = sv[1];
	fds[n++] = socket(AF_INET, SOCK_STREAM, 0);
	fds[n++] = socket(AF_INET, SOCK_DGRAM, 0);
	fds[n++] = socket(AF_INET6, SOCK_DGRAM, 0);
	fds[n++] = socket(AF_NETLINK, SOCK_RAW, 0);
	fds[n++] = socket(AF_UNIX, SOCK_STREAM, 0);
	fds[n++] = eventfd(1, EFD_NONBLOCK);
	fds[n++] = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
	fds[n++] = epoll_create1(0);
	fds[n++] = inotify_init1(IN_NONBLOCK);
	sigset_t m; sigemptyset(&m); sigaddset(&m, SIGUSR1);
	fds[n++] = signalfd(-1, &m, SFD_NONBLOCK);
	fds[n++] = (int)syscall(SYS_memfd_create, "fz", 0);
	fds[n++] = (int)syscall(SYS_io_uring_setup, 8, buf_rw + 1024);
	(void)n;
}

static void child_body(uint64_t seed, unsigned batch, int threads)
{
	/* a blocked call comes back EINTR, not at the end of the batch watchdog */
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_alarm;
	sigaction(SIGALRM, &sa, NULL);
	struct itimerval it = { {0, 20000}, {0, 20000} };
	setitimer(ITIMER_REAL, &it, NULL);

	buf_rw   = mmap(NULL, 1 << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	buf_ro   = mmap(NULL, 4096, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	buf_none = mmap(NULL, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	buf_edge = mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	munmap(buf_edge + 4096, 4096);
	fill_pool();

	/* a bounded address space keeps the worst random mmap from reaching the OOM
	 * killer, which would pick off unrelated processes */
	struct rlimit rl = { 1UL << 30, 1UL << 30 };
	setrlimit(RLIMIT_AS, &rl);

	/* structs the kernel will parse: plausible more often than not */
	rng_t r0 = { seed * 0x100000001b3ULL + batch };
	for (int i = 0; i < (1 << 20) / 8; i++) ((uint64_t *)buf_rw)[i] = arg_any(&r0);

	static rng_t r2;
	r2.s = r0.s ^ 0xabcdef;
	pthread_t t;
	int have_t = threads && pthread_create(&t, NULL, runner, &r2) == 0;
	runner(&r0);
	if (have_t) pthread_join(t, NULL);
	_exit(0);
}

static void prepare_dir(void)
{
	char cmd[64];
	chmod(WORKDIR, 0755);   /* a batch may have shut it (fchmod 000) */
	snprintf(cmd, sizeof(cmd), "rm -rf %s", WORKDIR);
	if (system(cmd)) { /* a leftover only makes the batch less tidy */ }
	if (mkdir(WORKDIR, 0755) != 0) {
		/* still there: every later batch would chdir into the old one and
		 * make no calls at all. Say why once, then move it aside. */
		static int noted, n;
		if (!noted++) {
			int r = rmdir(WORKDIR);
			printf("FUZZ-NOTE workdir stayed: rmdir=%d errno=%d\n", r, r ? errno : 0);
		}
		char old[64];
		snprintf(old, sizeof(old), "%s.old%d", WORKDIR, n++);
		rename(WORKDIR, old);
		mkdir(WORKDIR, 0755);
	}
	mkdir(WORKDIR "/d", 0755);
	int fd = open(WORKDIR "/a", O_RDWR | O_CREAT, 0644);
	if (fd >= 0) { (void)!write(fd, "fuzz", 4); close(fd); }
	close(open(WORKDIR "/f", O_RDWR | O_CREAT, 0644));
	(void)!symlink(WORKDIR "/a", WORKDIR "/l");
	chown(WORKDIR, UID_FUZZ, UID_FUZZ);
	chown(WORKDIR "/d", UID_FUZZ, UID_FUZZ);
	lchown(WORKDIR "/a", UID_FUZZ, UID_FUZZ);   /* a link left here must not lead root elsewhere */
	lchown(WORKDIR "/f", UID_FUZZ, UID_FUZZ);
}

/* 1 reaped, 0 still there after ms */
static int wait_for(pid_t p, int *st, int ms)
{
	for (int i = 0; i < ms / 10; i++) {
		pid_t w = waitpid(p, st, WNOHANG);
		if (w == p || (w < 0 && errno == ECHILD)) return 1;
		usleep(10000);
	}
	return 0;
}

int main(int argc, char **argv)
{
	uint64_t seed = argc > 1 ? strtoull(argv[1], NULL, 0) : 1;
	unsigned batches = argc > 2 ? (unsigned)strtoul(argv[2], NULL, 0) : 100;
	int threads = argc > 3 ? atoi(argv[3]) : 0;
	long only = argc > 4 ? atol(argv[4]) : -1;
	unshare(CLONE_NEWIPC);   /* the queues it makes go with the namespace, not fill the table */
	verbose = getenv("FUZZ_VERBOSE") != NULL;
	if (getenv("FUZZ_RANGE")) sscanf(getenv("FUZZ_RANGE"), "%d-%d", &range_lo, &range_hi);

	calls_made = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	unsigned killed = 0, signalled = 0, done = 0;
	uint64_t t0 = (uint64_t)time(NULL);
	printf("FUZZ-START seed=%llu batches=%u threads=%d\n", (unsigned long long)seed, batches, threads);
	fflush(stdout);
	for (unsigned b = 0; b < batches; b++) {
		if (only >= 0 && (long)b != only) continue;
		if (getuid() == 0) prepare_dir();
		if (b % 20 == 0) { printf("FUZZ-BATCH seed=%llu b=%u\n", (unsigned long long)seed, b); fflush(stdout); }
		pid_t p = fork();
		if (p < 0) { usleep(100000); continue; }
		if (p == 0) {
			setgid(UID_FUZZ);
			setgroups(0, NULL);
			setuid(UID_FUZZ);
			if (chdir(WORKDIR)) _exit(1);
			setsid();
			prctl(PR_SET_PDEATHSIG, SIGKILL);   /* a timed-out parent must not leave it holding stdout */
			child_body(seed, b, threads);
		}
		int st = 0;
		if (!wait_for(p, &st, 8000)) {
			kill(p, SIGKILL);
			killed++;
			if (!wait_for(p, &st, 15000)) {
				printf("FUZZ-HANG seed=%llu b=%u pid=%d threads=%d\n",
				       (unsigned long long)seed, b, (int)p, threads);
				char path[64], line[256];
				snprintf(path, sizeof(path), "/proc/%d/status", (int)p);
				FILE *f = fopen(path, "r");
				while (f && fgets(line, sizeof(line), f))
					if (!strncmp(line, "State", 5) || !strncmp(line, "SigPnd", 6) ||
					    !strncmp(line, "SigBlk", 6) || !strncmp(line, "Threads", 7))
						fputs(line, stdout);
				fflush(stdout);
				if (getenv("FUZZ_LINGER")) sleep((unsigned)atoi(getenv("FUZZ_LINGER")));
				return 1;
			}
		} else if (WIFSIGNALED(st)) {
			signalled++;
		}
		done++;
	}
	printf("FUZZ-OK seed=%llu batches=%u calls=%llu signalled=%u watchdog=%u secs=%llu\n",
	       (unsigned long long)seed, done, (unsigned long long)*calls_made,
	       signalled, killed, (unsigned long long)((uint64_t)time(NULL) - t0));
	puts("OK");
	return 0;
}
