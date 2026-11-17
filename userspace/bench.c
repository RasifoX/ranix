/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/*
 * bench: the speed baseline (compared against Linux on the same machine). The same static binary runs on
 * Linux and on Ranix, on the same machine, and prints one line per measure:
 *   BENCH <name> <median ns/op> <min ns/op> <runs>
 * Time is CLOCK_MONOTONIC (the vDSO on both), the median of RUNS rounds.
 * Build: the userspace rule, or cc -O2 -static -o bench bench.c on Linux. tests/bench.py runs it in KVM.
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <signal.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <time.h>
#include <unistd.h>

#define RUNS 7

static int64_t ns(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (int64_t)t.tv_sec * 1000000000LL + t.tv_nsec;
}

static int cmp(const void *a, const void *b)
{
	int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;
	return x < y ? -1 : x > y;
}

typedef void (*fn)(long n);

static void measure(const char *name, fn f, long n)
{
	int64_t r[RUNS];
	f(n / 10 + 1);                      /* warm up */
	for (int i = 0; i < RUNS; i++) {
		int64_t t0 = ns();
		f(n);
		r[i] = (ns() - t0) / n;
	}
	qsort(r, RUNS, sizeof r[0], cmp);
	printf("BENCH %-14s %10lld %10lld %d\n", name, (long long)r[RUNS / 2], (long long)r[0], RUNS);
	fflush(stdout);
}

static void b_syscall(long n) { for (long i = 0; i < n; i++) syscall(SYS_getppid); }

static void b_clock(long n)
{
	struct timespec t;
	for (long i = 0; i < n; i++) clock_gettime(CLOCK_MONOTONIC, &t);
}

static void b_openclose(long n)
{
	for (long i = 0; i < n; i++) close(open("/proc/self/stat", O_RDONLY));
}

static char tmpf[64];

static void b_stat(long n)
{
	struct stat st;
	for (long i = 0; i < n; i++) stat(tmpf, &st);
}

static void b_read4k(long n)
{
	static char b[4096];
	int fd = open(tmpf, O_RDONLY);
	for (long i = 0; i < n; i++) pread(fd, b, sizeof b, 0);
	close(fd);
}

static void b_write4k(long n)
{
	static char b[4096];
	int fd = open(tmpf, O_WRONLY);
	for (long i = 0; i < n; i++) pwrite(fd, b, sizeof b, 0);
	close(fd);
}

/* one round trip = two context switches between two processes on one CPU */
static void b_ctxsw(long n)
{
	int a[2], b[2];
	char c = 0;
	if (pipe(a) || pipe(b)) exit(1);
	pid_t p = fork();
	if (p == 0) {
		cpu_set_t s; CPU_ZERO(&s); CPU_SET(0, &s); sched_setaffinity(0, sizeof s, &s);
		for (long i = 0; i < n; i++) { if (read(a[0], &c, 1) != 1) _exit(1); if (write(b[1], &c, 1) != 1) _exit(1); }
		_exit(0);
	}
	cpu_set_t s; CPU_ZERO(&s); CPU_SET(0, &s); sched_setaffinity(0, sizeof s, &s);
	for (long i = 0; i < n; i++) { if (write(a[1], &c, 1) != 1) break; if (read(b[0], &c, 1) != 1) break; }
	waitpid(p, 0, 0);
	close(a[0]); close(a[1]); close(b[0]); close(b[1]);
}

static void b_fork(long n)
{
	for (long i = 0; i < n; i++) {
		pid_t p = fork();
		if (p == 0) _exit(0);
		waitpid(p, 0, 0);
	}
}

static const char *self;

static void b_forkexec(long n)
{
	for (long i = 0; i < n; i++) {
		pid_t p = fork();
		if (p == 0) { execl(self, self, "-x", (char *)0); _exit(1); }
		waitpid(p, 0, 0);
	}
}

/* 1 MB anonymous: map, touch every page (the faults), unmap */
static void b_mmap(long n)
{
	for (long i = 0; i < n; i++) {
		char *m = mmap(0, 1 << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (m == MAP_FAILED) exit(1);
		for (int k = 0; k < (1 << 20); k += 4096) m[k] = 1;
		munmap(m, 1 << 20);
	}
}

static void b_pipebw(long n)        /* n x 64 KB through a pipe to a child */
{
	int a[2];
	static char b[65536];
	if (pipe(a)) exit(1);
	pid_t p = fork();
	if (p == 0) {
		close(a[1]);
		while (read(a[0], b, sizeof b) > 0) { }
		_exit(0);
	}
	close(a[0]);
	for (long i = 0; i < n; i++) {
		size_t o = 0;
		while (o < sizeof b) { ssize_t w = write(a[1], b + o, sizeof b - o); if (w <= 0) break; o += (size_t)w; }
	}
	close(a[1]);
	waitpid(p, 0, 0);
}

static void b_tcprtt(long n)        /* 1-byte round trip over loopback TCP */
{
	int l = socket(AF_INET, SOCK_STREAM, 0), one = 1;
	struct sockaddr_in sa = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
	socklen_t sl = sizeof sa;
	if (l < 0 || bind(l, (void *)&sa, sizeof sa) || listen(l, 1) || getsockname(l, (void *)&sa, &sl)) { printf("BENCH tcp_rtt yok\n"); return; }
	pid_t p = fork();
	if (p == 0) {
		int c = accept(l, 0, 0);
		char x;
		setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
		while (read(c, &x, 1) == 1) if (write(c, &x, 1) != 1) break;
		_exit(0);
	}
	int c = socket(AF_INET, SOCK_STREAM, 0);
	char x = 0;
	if (connect(c, (void *)&sa, sizeof sa)) { printf("BENCH tcp_rtt yok\n"); kill(p, 9); waitpid(p, 0, 0); return; }
	setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
	for (long i = 0; i < n; i++) { if (write(c, &x, 1) != 1 || read(c, &x, 1) != 1) break; }
	close(c); close(l);
	waitpid(p, 0, 0);
}

int main(int argc, char **argv)
{
	if (argc > 1 && !strcmp(argv[1], "-x")) return 0;
	self = argv[0];
	snprintf(tmpf, sizeof tmpf, "/tmp/hiz.%d", (int)getpid());
	int fd = open(tmpf, O_CREAT | O_RDWR | O_TRUNC, 0600);
	static char b[4096];
	if (fd < 0 || write(fd, b, sizeof b) != sizeof b) { perror("tmp"); return 1; }
	close(fd);
	struct utsname_like { char s[65], n[65], r[65], v[65], m[65], d[65]; } u;
	if (syscall(SYS_uname, &u) == 0) printf("BENCH uname %s %s %s\n", u.s, u.r, u.m);
	static const struct { const char *name; fn f; long n; } all[] = {
		{ "syscall", b_syscall, 1000000 }, { "clock_vdso", b_clock, 5000000 },
		{ "open_close", b_openclose, 100000 }, { "stat", b_stat, 200000 },
		{ "read_4k", b_read4k, 200000 }, { "write_4k", b_write4k, 100000 },
		{ "ctxsw_rtt", b_ctxsw, 50000 }, { "fork_exit", b_fork, 2000 },
		{ "fork_exec", b_forkexec, 1000 }, { "mmap_1m", b_mmap, 2000 },
		{ "pipe_64k", b_pipebw, 20000 }, { "tcp_rtt", b_tcprtt, 20000 },
	};
	/* bench NAME: that one only, ten times as long (for /dev/prof) */
	for (size_t i = 0; argc > 1 && i < sizeof all / sizeof all[0]; i++)
		if (!strcmp(argv[1], all[i].name)) { all[i].f(all[i].n * 10); unlink(tmpf); printf("BENCH bitti\n"); return 0; }
	for (size_t i = 0; i < sizeof all / sizeof all[0]; i++) measure(all[i].name, all[i].f, all[i].n);
	unlink(tmpf);
	printf("BENCH bitti\n");
	return 0;
}
