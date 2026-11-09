/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* A sleep never ends before the time asked for (POSIX; LTP nanosleep01,
 * clock_nanosleep02, prctl09 count "woken up early"): nanosleep and
 * clock_nanosleep of 0.3, 1 and 2.5 ms, measured on CLOCK_MONOTONIC. */
#include <stdio.h>
#include <time.h>

static long long ns(struct timespec t) { return t.tv_sec * 1000000000LL + t.tv_nsec; }

int main(void)
{
	int bad = 0;
	long want[] = { 300000, 1000000, 2500000 };
	for (int w = 0; w < 3; w++) {
		int early = 0;
		long long least = -1;
		for (int i = 0; i < 100; i++) {
			struct timespec a, b, d = { 0, want[w] };
			clock_gettime(CLOCK_MONOTONIC, &a);
			if (i % 2) nanosleep(&d, NULL);
			else clock_nanosleep(CLOCK_MONOTONIC, 0, &d, NULL);
			clock_gettime(CLOCK_MONOTONIC, &b);
			long long got = ns(b) - ns(a);
			if (got < want[w]) { early++; if (least < 0 || got < least) least = got; }
		}
		if (early) { printf("%ld ns sleep ended early %d of 100 times (shortest %lld ns)\n", want[w], early, least); bad++; }
	}
	puts(bad ? "FAIL" : "OK");
	return bad != 0;
}
