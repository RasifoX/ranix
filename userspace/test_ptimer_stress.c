/* timer_create under load: many short one-shot SIGEV_SIGNAL timers, each armed
 * just after the expiry thread's scan that fired a "kick" timer, when that thread
 * is about to go idle. First in one process, then in one process per CPU, each
 * after a child that caught a SIGSEGV. The waiting is nanosleep, as c-ptimer does.
 * A one-shot whose signal misses value + GRACE_US is lost; it is then waited for
 * up to LATE_US, so a late one (the idle thread sleeps a second) shows its delay.
 * Usage: test_ptimer_stress [iterations [value_us [workers]]] */
#define _GNU_SOURCE
#include <sched.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define KICK_VAL  1
#define SHOT_VAL  2
#define MAX_LOST  5               /* each lost one costs up to LATE_US */
#define GRACE_US  100000LL
#define LATE_US   1500000LL
#define SPIN_LO   600             /* arm this long after the kick, in us ... */
#define SPIN_SPAN 700             /* ... swept over this span: one tick plus margin */

static volatile sig_atomic_t kicks, shots, bad;
static volatile long long shot_at;
static int me;                    /* worker number, in the values: a stray signal shows */
static sigjmp_buf jb;

static long long now_us(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000000LL + t.tv_nsec / 1000;
}

static void on_timer(int s, siginfo_t *si, void *u)
{
	(void)s; (void)u;
	int v = si->si_value.sival_int;
	if (si->si_code != SI_TIMER) bad++;
	else if (v == KICK_VAL + 16 * me) kicks++;
	else if (v == SHOT_VAL + 16 * me) { shot_at = now_us(); shots++; }
	else bad++;
}

static void on_segv(int s) { (void)s; siglongjmp(jb, 1); }

/* what c-faultsig does before c-ptimer: a child that handles its own SIGSEGV */
static void segv_child(void)
{
	pid_t c = fork();
	if (c == 0) {
		struct sigaction sa;
		memset(&sa, 0, sizeof sa);
		sa.sa_handler = on_segv;
		sigaction(SIGSEGV, &sa, NULL);
		if (!sigsetjmp(jb, 1)) *(volatile int *)(uintptr_t)16 = 1;
		_exit(0);
	}
	int st;
	if (c > 0) waitpid(c, &st, 0);
}

/* 1 once *flag moved from seen, 0 at the deadline. nanosleep is cut by the signal. */
static int wait_for(volatile sig_atomic_t *flag, int seen, long long deadline)
{
	for (;;) {
		if (*flag != seen) return 1;
		long long left = deadline - now_us();
		if (left <= 0) return 0;
		struct timespec t = { left / 1000000, left % 1000000 * 1000 };
		nanosleep(&t, NULL);
	}
}

static int make_timer(int val, timer_t *t)
{
	struct sigevent ev;
	memset(&ev, 0, sizeof ev);
	ev.sigev_notify = SIGEV_SIGNAL;
	ev.sigev_signo = SIGRTMIN + 2;
	ev.sigev_value.sival_int = val + 16 * me;
	return timer_create(CLOCK_MONOTONIC, &ev, t);
}

typedef struct { int done, lost_kick, lost_shot, never; long long max_ok, max_late; } tally_t;

/* One iteration: kick fires, spin into the next tick's scan, arm the shot. */
static void one(timer_t kick, timer_t shot, long value_us, int i, tally_t *r)
{
	struct itimerspec k1 = { {0, 0}, {0, 1000000} };
	struct itimerspec s1 = { {0, 0}, {value_us / 1000000, value_us % 1000000 * 1000} };
	int k = kicks;
	long long t0 = now_us();
	timer_settime(kick, 0, &k1, NULL);
	if (!wait_for(&kicks, k, t0 + 1000 + GRACE_US)) {
		r->lost_kick++;
		wait_for(&kicks, k, t0 + LATE_US);
		return;
	}
	long long until = now_us() + SPIN_LO + (i * 7) % SPIN_SPAN;
	while (now_us() < until) ;
	int s = shots;
	long long armed = now_us();
	timer_settime(shot, 0, &s1, NULL);
	if (wait_for(&shots, s, armed + value_us + GRACE_US)) {
		if (shot_at - armed > r->max_ok) r->max_ok = shot_at - armed;
		return;
	}
	r->lost_shot++;
	if (!wait_for(&shots, s, armed + LATE_US)) {
		struct itimerspec off;
		memset(&off, 0, sizeof off);
		timer_settime(shot, 0, &off, NULL);   /* a later arrival would count for the next one */
		r->never++;
	}
	else if (shot_at - armed > r->max_late) r->max_late = shot_at - armed;
}

static int run(int iters, long value_us, const char *name)
{
	timer_t shot, kick;
	tally_t r;
	memset(&r, 0, sizeof r);
	segv_child();
	/* the shot first: the lower slot, scanned early, leaves the widest window */
	if (make_timer(SHOT_VAL, &shot) || make_timer(KICK_VAL, &kick)) {
		printf("%s: timer_create failed\n", name);
		return 1;
	}
	for (r.done = 0; r.done < iters && r.lost_kick + r.lost_shot < MAX_LOST; r.done++)
		one(kick, shot, value_us, r.done, &r);
	timer_delete(shot);
	timer_delete(kick);
	printf("%s: %d iterations of %ld us, lost kick %d shot %d (never %d), "
	       "max on time %lld us, max late %lld us, bad %d\n", name, r.done, value_us,
	       r.lost_kick, r.lost_shot, r.never, r.max_ok, r.max_late, (int)bad);
	fflush(stdout);
	return r.lost_kick || r.lost_shot || bad;
}

static int workers(int n, int iters, long value_us)
{
	int fail = 0;
	long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
	for (int w = 1; w <= n; w++) {
		if (fork() != 0) continue;
		cpu_set_t set;
		CPU_ZERO(&set);
		CPU_SET((w - 1) % (ncpu > 0 ? ncpu : 1), &set);
		sched_setaffinity(0, sizeof set, &set);
		me = w;
		char name[32];
		snprintf(name, sizeof name, "cpu%ld", (long)((w - 1) % (ncpu > 0 ? ncpu : 1)));
		_exit(run(iters, value_us, name));
	}
	for (int w = 0; w < n; w++) {
		int st;
		if (wait(&st) < 0 || !WIFEXITED(st) || WEXITSTATUS(st)) fail = 1;
	}
	return fail;
}

int main(int argc, char **argv)
{
	int iters = argc > 1 ? atoi(argv[1]) : 2000;
	long value_us = argc > 2 ? atol(argv[2]) : 2000;
	long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
	int n = argc > 3 ? atoi(argv[3]) : (ncpu > 0 ? (int)ncpu : 1);
	if (iters < 1 || value_us < 1 || n < 1) return 2;

	struct sigaction sa;
	memset(&sa, 0, sizeof sa);
	sa.sa_sigaction = on_timer;
	sa.sa_flags = SA_SIGINFO | SA_RESTART;
	sigaction(SIGRTMIN + 2, &sa, NULL);
	setvbuf(stdout, NULL, _IOLBF, 0);

	int fail = run(iters, value_us, "one");
	fail |= workers(n, iters / n > 0 ? iters / n : 1, value_us);
	puts(fail ? "FAIL" : "OK");
	return fail;
}
