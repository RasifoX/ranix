/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* CPU-bound tasks share the CPUs fairly and none starves, also next to tasks
 * that sleep and wake all the time (what wakeup preemption must not break).
 * Every bound is relative to the same run, so other load on the machine does
 * not decide it.
 *  A: 2 loops per CPU for 2 s: the slowest loop does at least 0.6 of the mean
 *     and at least 0.5 of the fastest.
 *  B: per CPU one loop and one "almost busy" task (2 ms of work, a 100 us
 *     sleep, again) for 2 s: each kind gets at least half of what the other
 *     gets, and every task at least 0.4 of the mean.
 *  C: per CPU one loop and two light tasks (1 ms sleep, a little work) for 2 s:
 *     the loops keep at least 0.75 of what they do with no light tasks beside
 *     them (the wakeups cost little), and every light task keeps its rhythm
 *     (at least half the wakeups it gets alone).
 * Linux (CFS/EEVDF) meets all of these with room. */
#define _GNU_SOURCE
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAXT 256

struct shm {
	volatile int go, stop;
	volatile uint64_t work[MAXT];
	volatile uint64_t wakes[MAXT];
};
static struct shm *S;

static long long now_ns(void)
{
	struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000000000LL + t.tv_nsec;
}

static void unit(void)   /* ~a few us of work */
{
	for (volatile int i = 0; i < 2000; i++) ;
}

enum { LOOP, BUSY, LIGHT };

static void child(int idx, int kind)
{
	prctl(PR_SET_PDEATHSIG, SIGKILL);
	alarm(30);
	while (!S->go) ;
	while (!S->stop) {
		if (kind == LOOP) { unit(); S->work[idx]++; continue; }
		if (kind == BUSY) {
			long long end = now_ns() + 2000000;
			while (now_ns() < end && !S->stop) { unit(); S->work[idx]++; }
			struct timespec d = { 0, 100000 };
			nanosleep(&d, NULL);
		} else {
			struct timespec d = { 0, 1000000 };
			nanosleep(&d, NULL);
			for (int k = 0; k < 10; k++) { unit(); S->work[idx]++; }
			S->wakes[idx]++;
		}
	}
	_exit(0);
}

static int run(int n, const int *kinds, double secs)
{
	S->go = 0; S->stop = 0;
	for (int i = 0; i < n; i++) { S->work[i] = 0; S->wakes[i] = 0; }
	pid_t kids[MAXT];
	for (int i = 0; i < n; i++) {
		kids[i] = fork();
		if (kids[i] == 0) child(i, kinds[i]);
		if (kids[i] < 0) { puts("fork failed"); return -1; }
	}
	struct timespec settle = { 0, 50000000 };
	nanosleep(&settle, NULL);
	S->go = 1;
	struct timespec d = { (time_t)secs, (long)((secs - (time_t)secs) * 1e9) };
	nanosleep(&d, NULL);
	S->stop = 1;
	for (int i = 0; i < n; i++) waitpid(kids[i], NULL, 0);
	return 0;
}

int main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
	if (ncpu < 1) ncpu = 1;
	if (ncpu > 64) ncpu = 64;
	S = mmap(NULL, sizeof(*S), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (S == MAP_FAILED) { puts("mmap failed"); puts("FAIL"); return 1; }
	int kinds[MAXT];
	int bad = 0;

	/* R and the light tasks' alone rhythm: one task of each, 0.5 s */
	kinds[0] = LOOP; kinds[1] = LIGHT;
	if (run(2, kinds, 0.5)) { puts("FAIL"); return 1; }
	double R = S->work[0] / 0.5;
	double light_alone = S->wakes[1] / 0.5;
	printf("alone: %.0f units/s, light task %.0f wakeups/s\n", R, light_alone);
	if (R < 1000 || light_alone < 50) { puts("calibration failed"); puts("FAIL"); return 1; }

	/* A */
	int n = (int)ncpu * 2;
	for (int i = 0; i < n; i++) kinds[i] = LOOP;
	if (run(n, kinds, 2.0)) { puts("FAIL"); return 1; }
	{
		double mn = 1e30, mx = 0, mean = 0;
		for (int i = 0; i < n; i++) {
			double w = (double)S->work[i];
			mean += w;
			if (w < mn) mn = w;
			if (w > mx) mx = w;
		}
		mean /= n;
		printf("A %d loops on %ld CPUs: slowest %.2f, fastest %.2f of the mean (each %.2f of a CPU)\n",
		       n, ncpu, mn / mean, mx / mean, mean / (R * 2.0));
		if (mn < 0.6 * mean || mn < 0.5 * mx) { puts("A: unfair or starved"); bad++; }
	}

	/* B */
	n = (int)ncpu * 2;
	for (int i = 0; i < n; i++) kinds[i] = (i & 1) ? BUSY : LOOP;
	if (run(n, kinds, 2.0)) { puts("FAIL"); return 1; }
	{
		double sl = 0, sb = 0, mn = 1e30;
		for (int i = 0; i < n; i++) {
			double w = (double)S->work[i];
			if (i & 1) sb += w; else sl += w;
			if (w < mn) mn = w;
		}
		double mean = (sl + sb) / n;
		sl /= ncpu; sb /= ncpu;
		printf("B %ld loops + %ld almost-busy: loops %.2f, almost-busy %.2f of the mean, slowest task %.2f\n",
		       ncpu, ncpu, sl / mean, sb / mean, mn / mean);
		if (sl < 0.5 * sb || sb < 0.5 * sl || mn < 0.4 * mean) { puts("B: unfair or starved"); bad++; }
	}

	/* C: the loops alone first, then with the light tasks */
	n = (int)ncpu;
	for (int i = 0; i < n; i++) kinds[i] = LOOP;
	if (run(n, kinds, 1.0)) { puts("FAIL"); return 1; }
	double l0 = 0;
	for (int i = 0; i < n; i++) l0 += (double)S->work[i];
	l0 /= ncpu;                      /* per loop per second */
	n = (int)ncpu * 3;
	for (int i = 0; i < n; i++) kinds[i] = (i % 3) ? LIGHT : LOOP;
	if (run(n, kinds, 2.0)) { puts("FAIL"); return 1; }
	{
		double sl = 0, mnw = 1e30;
		for (int i = 0; i < n; i++) {
			if (i % 3) { double w = S->wakes[i] / 2.0; if (w < mnw) mnw = w; }
			else sl += (double)S->work[i];
		}
		sl /= ncpu * 2.0;
		printf("C %ld loops + %ld light: loops keep %.2f of their pace alone, slowest light task %.0f wakeups/s (alone %.0f)\n",
		       ncpu, ncpu * 2, l0 ? sl / l0 : 0, mnw, light_alone);
		if (sl < 0.75 * l0) { puts("C: loops lost too much to the light tasks"); bad++; }
		if (mnw < 0.5 * light_alone) { puts("C: a light task lost its rhythm"); bad++; }
	}

	puts(bad ? "FAIL" : "OK");
	return bad != 0;
}
