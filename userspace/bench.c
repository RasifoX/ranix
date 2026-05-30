/*
 * bench — Ranix micro-benchmark suite.
 *
 * Measures the kernel fast paths that decide "does Ranix flow":
 * syscall latency, context-switch latency, fork/exec throughput, pipe
 * bandwidth, mmap and page-fault cost. Prints one "BENCH:" line per metric
 * so results can be diffed against Linux on the same hardware.
 *
 * Timing uses the ARM generic virtual counter (cntvct_el0), readable from
 * EL0, giving sub-microsecond resolution — unlike the 10ms tick clock.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sched.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <sys/syscall.h>

#if defined(__aarch64__)
static inline uint64_t now_ticks(void)
{
	uint64_t v;
	__asm__ volatile("mrs %0, cntvct_el0" : "=r"(v));
	return v;
}
static inline uint64_t tick_hz(void)
{
	uint64_t f;
	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
	return f;
}
#elif defined(__x86_64__)
static inline uint64_t now_ticks(void)
{
	uint32_t lo, hi;
	__asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
	return ((uint64_t)hi << 32) | lo;
}
static uint64_t tick_hz(void) { return 0; /* TSC freq unknown; report raw */ }
#endif

static uint64_t g_hz;

static uint64_t ticks_to_ns(uint64_t t)
{
	if (!g_hz) return t; /* unknown freq: raw ticks */
	return (t * 1000000000ULL) / g_hz;
}

static void report(const char *name, uint64_t total_ticks, uint64_t iters)
{
	uint64_t ns = ticks_to_ns(total_ticks) / (iters ? iters : 1);
	printf("BENCH: %-18s %8llu ns/op   (%llu ops)\n",
	       name, (unsigned long long)ns, (unsigned long long)iters);
	fflush(stdout);
}

/* ── syscall latency: getpid in a tight loop ────────────────── */
static void bench_syscall(void)
{
	const uint64_t N = 20000;
	uint64_t t0 = now_ticks();
	for (uint64_t i = 0; i < N; i++)
		(void)syscall(SYS_getpid);
	report("syscall-getpid", now_ticks() - t0, N);
}

/* ── scheduler round-trip: sched_yield ──────────────────────── */
static void bench_yield(void)
{
	const uint64_t N = 100;
	uint64_t t0 = now_ticks();
	for (uint64_t i = 0; i < N; i++)
		sched_yield();
	report("sched-yield", now_ticks() - t0, N);
}

/* ── fork + immediate exit + wait ───────────────────────────── */
static void bench_fork(void)
{
	const uint64_t N = 30;
	uint64_t t0 = now_ticks();
	for (uint64_t i = 0; i < N; i++) {
		pid_t p = fork();
		if (p == 0) _exit(0);
		if (p > 0) { int st; waitpid(p, &st, 0); }
	}
	report("fork+wait", now_ticks() - t0, N);
}

/* ── fork + execve(/bin/true) + wait ────────────────────────── */
static void bench_forkexec(void)
{
	const uint64_t N = 20;
	char *argv[] = { "/bin/true", NULL };
	char *envp[] = { NULL };
	uint64_t t0 = now_ticks();
	for (uint64_t i = 0; i < N; i++) {
		pid_t p = fork();
		if (p == 0) { execve("/bin/true", argv, envp); _exit(127); }
		if (p > 0) { int st; waitpid(p, &st, 0); }
	}
	report("fork+exec+wait", now_ticks() - t0, N);
}

/* ── context-switch via pipe ping-pong between two processes ── */
static void bench_ctxswitch(void)
{
	const uint64_t N = 100;
	int a[2], b[2];
	if (pipe(a) < 0 || pipe(b) < 0) return;
	pid_t p = fork();
	if (p == 0) {
		char c;
		for (uint64_t i = 0; i < N; i++) {
			if (read(a[0], &c, 1) != 1) _exit(1);
			if (write(b[1], &c, 1) != 1) _exit(1);
		}
		_exit(0);
	}
	char c = 'x';
	uint64_t t0 = now_ticks();
	for (uint64_t i = 0; i < N; i++) {
		write(a[1], &c, 1);
		read(b[0], &c, 1);
	}
	uint64_t dt = now_ticks() - t0;
	int st; waitpid(p, &st, 0);
	close(a[0]); close(a[1]); close(b[0]); close(b[1]);
	/* each round = 2 context switches (parent→child→parent) */
	report("ctxsw (pipe rt)", dt, N * 2);
}

/* ── pipe throughput ────────────────────────────────────────── */
static void bench_pipe_bw(void)
{
	const uint64_t ROUNDS = 1000;
	const int SZ = 4096;
	static char buf[4096];
	int fd[2];
	if (pipe(fd) < 0) return;
	pid_t p = fork();
	if (p == 0) {
		for (uint64_t i = 0; i < ROUNDS; i++) {
			int off = 0;
			while (off < SZ) { int r = write(fd[1], buf + off, SZ - off); if (r <= 0) _exit(1); off += r; }
		}
		_exit(0);
	}
	close(fd[1]);
	uint64_t t0 = now_ticks();
	uint64_t got = 0;
	for (uint64_t i = 0; i < ROUNDS; i++) {
		int off = 0;
		while (off < SZ) { int r = read(fd[0], buf + off, SZ - off); if (r <= 0) break; off += r; }
		got += off;
	}
	uint64_t dt_ns = ticks_to_ns(now_ticks() - t0);
	int st; waitpid(p, &st, 0); close(fd[0]);
	uint64_t mb = got / (1024 * 1024);
	uint64_t mbps = dt_ns ? (mb * 1000000000ULL) / dt_ns : 0;
	printf("BENCH: %-18s %8llu MB/s   (%llu MB)\n",
	       "pipe-bw", (unsigned long long)mbps, (unsigned long long)mb);
	fflush(stdout);
}

/* ── mmap + fault + munmap ──────────────────────────────────── */
static void bench_mmap(void)
{
	const uint64_t N = 2000;
	const size_t SZ = 64 * 1024;
	uint64_t t0 = now_ticks();
	for (uint64_t i = 0; i < N; i++) {
		void *m = mmap(0, SZ, PROT_READ | PROT_WRITE,
		               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (m == MAP_FAILED) break;
		*(volatile char *)m = 1;          /* fault one page in */
		munmap(m, SZ);
	}
	report("mmap+fault+unmap", now_ticks() - t0, N);
}

int main(void)
{
	g_hz = tick_hz();
	printf("BENCH: timer_hz=%llu\n", (unsigned long long)g_hz);
	fflush(stdout);

	bench_syscall();
	bench_yield();
	bench_fork();
	bench_forkexec();
	bench_ctxswitch();
	bench_pipe_bw();
	bench_mmap();

	printf("BENCH: done\n");
	fflush(stdout);
	return 0;
}
