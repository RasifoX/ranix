/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* Real-time tasks and wakeups (SCHED_FIFO semantics next to wakeup preemption):
 *  1. With every CPU busy running a normal CPU-bound loop, a SCHED_FIFO task
 *     sleeping 100 us 150 times runs again at once: median and p90 of how late
 *     it is under 1 ms (Linux: tens of us).
 *  2. A SCHED_FIFO task spinning on CPU 0 is not preempted by a normal task
 *     woken there: a normal task pinned to CPU 0 that sleeps 1 ms in a loop
 *     gets at most 2 wakeups through a 300 ms spin (Linux: 0; RT throttling
 *     only steps in after 950 ms of a second).
 *  3. Nor by a SCHED_FIFO task of the same priority (at most 2 wakeups).
 *  4. A higher-priority SCHED_FIFO task woken there does preempt it: at least
 *     100 wakeups (Linux: ~270). */
#define _GNU_SOURCE
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define N 150

static long long now_ns(void)
{
	struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000000000LL + t.tv_nsec;
}

static int cmp(const void *a, const void *b)
{
	long long x = *(const long long *)a, y = *(const long long *)b;
	return x < y ? -1 : x > y;
}

/* Raw syscall: musl's sched_setscheduler wrapper returns ENOSYS by design */
static int set_policy(int policy, int prio)
{
	struct sched_param sp = { .sched_priority = prio };
	return (int)syscall(SYS_sched_setscheduler, 0, policy, &sp);
}

static volatile uint64_t *wakes;

/* This task (pinned to CPU 0) spins 300 ms as SCHED_FIFO 10 while a child pinned
 * there too, of the given policy, sleeps 1 ms in a loop. Returns the child's
 * wakeups during the spin, -1 on a setup failure. */
static long beside_fifo(int policy, int prio)
{
	static const char *const pol[] = { "normal", "SCHED_FIFO", "SCHED_RR" };
	*wakes = 0;
	pid_t nk = fork();
	if (nk == 0) {
		prctl(PR_SET_PDEATHSIG, SIGKILL);
		alarm(30);
		if (policy != SCHED_OTHER && set_policy(policy, prio)) _exit(1);
		struct timespec d = { 0, 1000000 };
		for (;;) { nanosleep(&d, NULL); (*wakes)++; }
	}
	struct timespec settle = { 0, 100000000 };
	nanosleep(&settle, NULL);
	uint64_t alone = *wakes;
	long got = -1;
	if (alone < 20) puts("the pinned task did not run alone");
	else if (set_policy(SCHED_FIFO, 10)) puts("sched_setscheduler FIFO failed");
	else {
		uint64_t w0 = *wakes;
		long long end = now_ns() + 300000000;
		while (now_ns() < end) __asm__ volatile("" ::: "memory");
		got = (long)(*wakes - w0);
		set_policy(SCHED_OTHER, 0);
		printf("%s %d task pinned beside a spinning SCHED_FIFO 10 task: %ld wakeups in 300 ms (alone %llu in 100 ms)\n",
		       pol[policy], prio, got, (unsigned long long)alone);
	}
	kill(nk, SIGKILL);
	waitpid(nk, NULL, 0);
	return got;
}

int main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
	if (ncpu < 1) ncpu = 1;
	if (ncpu > 64) ncpu = 64;
	int bad = 0;

	/* 1 */
	pid_t kids[64];
	for (long i = 0; i < ncpu; i++) {
		kids[i] = fork();
		if (kids[i] == 0) { prctl(PR_SET_PDEATHSIG, SIGKILL); alarm(60); for (;;) __asm__ volatile("" ::: "memory"); }
	}
	struct timespec settle = { 0, 100000000 };
	nanosleep(&settle, NULL);
	pid_t sub = fork();
	if (sub == 0) {
		if (set_policy(SCHED_FIFO, 10)) { puts("sched_setscheduler FIFO failed"); _exit(1); }
		static long long late[N];
		struct timespec d = { 0, 100000 };
		for (int i = 0; i < N; i++) {
			long long a = now_ns();
			nanosleep(&d, NULL);
			late[i] = now_ns() - a - 100000;
		}
		qsort(late, N, sizeof(late[0]), cmp);
		printf("%ld busy loops: SCHED_FIFO 100 us sleep late by median %lld us, p90 %lld us, max %lld us\n",
		       ncpu, late[N / 2] / 1000, late[N * 9 / 10] / 1000, late[N - 1] / 1000);
		_exit(late[N / 2] >= 1000000 || late[N * 9 / 10] >= 1000000);
	}
	int st = 0;
	waitpid(sub, &st, 0);
	if (!WIFEXITED(st) || WEXITSTATUS(st)) bad++;
	for (long i = 0; i < ncpu; i++) kill(kids[i], SIGKILL);
	for (long i = 0; i < ncpu; i++) waitpid(kids[i], NULL, 0);

	/* 2, 3, 4: on CPU 0 only */
	wakes = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (wakes == MAP_FAILED) { puts("mmap failed"); puts("FAIL"); return 1; }
	cpu_set_t one; CPU_ZERO(&one); CPU_SET(0, &one);
	if (sched_setaffinity(0, sizeof(one), &one)) { puts("sched_setaffinity failed"); puts("FAIL"); return 1; }
	long got;
	if ((got = beside_fifo(SCHED_OTHER, 0)) < 0 || got > 2) bad++;
	if ((got = beside_fifo(SCHED_FIFO, 10)) < 0 || got > 2) bad++;
	if ((got = beside_fifo(SCHED_FIFO, 20)) < 0 || got < 100) bad++;

	puts(bad ? "FAIL" : "OK");
	return bad != 0;
}
