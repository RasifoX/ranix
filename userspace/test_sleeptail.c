/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* A sleep long enough for the CPU to halt still ends on time: 600 sleeps of
 * 2.5 ms, never early, and no more than 1 in 10 over 300 us late (the 90th
 * percentile under 300 us; LTP allows 400 us at the median).
 *
 * The CPU halts for such a sleep and is woken ahead of its end to spin the
 * rest (sched_idle_wait). In a guest on an idle host, the halted vCPU came
 * back 100-550 us late (Casper, 2026-11-11): with the margin learned too
 * briefly, 17-30 % of these sleeps ended ~0.5 ms late and c-sleepprec /
 * c-timerprec failed now and then in the full suite. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define N 600

static long long ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000000000LL + t.tv_nsec; }
static int cmp(const void *a, const void *b) { long long x = *(const long long *)a, y = *(const long long *)b; return x < y ? -1 : x > y; }

int main(void)
{
	static long long over[N];
	int early = 0, late = 0;
	for (int i = 0; i < N; i++) {
		struct timespec d = { 0, 2500000 };
		long long a = ns();
		clock_nanosleep(CLOCK_MONOTONIC, 0, &d, NULL);
		over[i] = ns() - a - 2500000;
		if (over[i] < 0) early++;
		if (over[i] > 300000) late++;
	}
	qsort(over, N, sizeof(over[0]), cmp);
	printf("2.5 ms sleep x%d: over 300 us late %d, p50 %lld us, p90 %lld us, max %lld us\n",
	       N, late, over[N / 2] / 1000, over[N * 9 / 10] / 1000, over[N - 1] / 1000);
	if (early) printf("woke early %d times\n", early);
	int bad = early || late > N / 10;
	puts(bad ? "FAIL" : "OK");
	return bad;
}
