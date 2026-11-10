/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* A sleep ends when it was asked to, not at the next tick: nanosleep and
 * clock_nanosleep of 0.3, 1 and 2.5 ms overshoot by under 400 us (LTP allows 400 us for a context switch) at the median
 * (LTP's timer tests: nanosleep01, clock_nanosleep02, prctl09, ...). And
 * clock_getres for CLOCK_MONOTONIC is no coarser than 400 us. */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static long long ns(struct timespec t) { return t.tv_sec * 1000000000LL + t.tv_nsec; }
static int cmp(const void *a, const void *b) { long long x = *(const long long *)a, y = *(const long long *)b; return x < y ? -1 : x > y; }

int main(void)
{
	int bad = 0;
	long want[] = { 300000, 1000000, 2500000 };
	static long long over[100];
	for (int w = 0; w < 3; w++) {
		for (int i = 0; i < 100; i++) {
			struct timespec a, b, d = { 0, want[w] };
			clock_gettime(CLOCK_MONOTONIC, &a);
			if (i % 2) nanosleep(&d, NULL);
			else clock_nanosleep(CLOCK_MONOTONIC, 0, &d, NULL);
			clock_gettime(CLOCK_MONOTONIC, &b);
			over[i] = ns(b) - ns(a) - want[w];
		}
		qsort(over, 100, sizeof(over[0]), cmp);
		if (over[50] > 400000) { printf("%ld ns sleep: median overshoot %lld ns\n", want[w], over[50]); bad++; }
	}
	struct timespec r;
	clock_getres(CLOCK_MONOTONIC, &r);
	if (ns(r) > 400000) { printf("CLOCK_MONOTONIC resolution %lld ns\n", ns(r)); bad++; }
	puts(bad ? "FAIL" : "OK");
	return bad != 0;
}
