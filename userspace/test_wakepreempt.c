/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* Wakeup preemption: with every CPU busy running a CPU-bound loop, a task that
 * slept (and so ran far less than the loops) runs again as soon as it is woken,
 * not after a loop's time slice. One busy loop per online CPU; 150 waits each of
 * a 1 us nanosleep, a 100 us timerfd and a 100 us POSIX timer (the last two are
 * woken through the kernel's timer thread, a second wakeup): how late they end
 * stays under 200 us at the median and 500 us at the 90th percentile (Linux and
 * Ranix with wakeup preemption: tens of us; without it Ranix was 0.1-0.9 ms
 * at the median, up to a tick, in KVM with kvm-clock). */
#define _GNU_SOURCE
#include <signal.h>
#include <sys/prctl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/timerfd.h>
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

static long long late[N];

static int verdict(long ncpu, const char *what)
{
	qsort(late, N, sizeof(late[0]), cmp);
	long long med = late[N / 2], p90 = late[N * 9 / 10], max = late[N - 1];
	printf("%ld busy loops: %s late by median %lld us, p90 %lld us, max %lld us\n",
	       ncpu, what, med / 1000, p90 / 1000, max / 1000);
	return med >= 200000 || p90 >= 500000;
}

int main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
	if (ncpu < 1) ncpu = 1;
	if (ncpu > 64) ncpu = 64;
	sigset_t set; sigemptyset(&set); sigaddset(&set, SIGRTMIN);
	sigprocmask(SIG_BLOCK, &set, NULL);
	pid_t kids[64];
	for (long i = 0; i < ncpu; i++) {
		kids[i] = fork();
		if (kids[i] == 0) { prctl(PR_SET_PDEATHSIG, SIGKILL); alarm(60); for (;;) __asm__ volatile("" ::: "memory"); }
	}
	struct timespec settle = { 0, 100000000 };   /* let the loops spread over the CPUs */
	nanosleep(&settle, NULL);
	int bad = 0;

	struct timespec us = { 0, 1000 };
	for (int i = 0; i < N; i++) {
		long long a = now_ns();
		nanosleep(&us, NULL);
		late[i] = now_ns() - a - 1000;
	}
	bad += verdict(ncpu, "1 us nanosleep");

	int fd = timerfd_create(CLOCK_MONOTONIC, 0);
	struct itimerspec its = { .it_value = { 0, 100000 } };
	for (int i = 0; i < N; i++) {
		long long a = now_ns();
		timerfd_settime(fd, 0, &its, NULL);
		uint64_t e;
		if (read(fd, &e, sizeof(e)) != sizeof(e)) { puts("timerfd read failed"); bad++; break; }
		late[i] = now_ns() - a - 100000;
	}
	bad += verdict(ncpu, "100 us timerfd");

	struct sigevent sev = { .sigev_notify = SIGEV_SIGNAL, .sigev_signo = SIGRTMIN };
	timer_t t;
	if (timer_create(CLOCK_MONOTONIC, &sev, &t)) { puts("timer_create failed"); bad++; }
	for (int i = 0; i < N; i++) {
		long long a = now_ns();
		timer_settime(t, 0, &its, NULL);
		sigwaitinfo(&set, NULL);
		late[i] = now_ns() - a - 100000;
	}
	bad += verdict(ncpu, "100 us POSIX timer");

	for (long i = 0; i < ncpu; i++) kill(kids[i], SIGKILL);
	for (long i = 0; i < ncpu; i++) waitpid(kids[i], NULL, 0);
	puts(bad ? "FAIL" : "OK");
	return bad != 0;
}
