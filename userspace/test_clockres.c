/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* clock_getres tells what a timer can do, as on Linux (a kernel without high
 * resolution timers says one tick): a 1 us nanosleep must not overshoot by
 * more than twice the reported resolution plus 0.5 ms (LTP's timer tests
 * size their limits from it: epoll_wait02, poll02, nanosleep01, ...).
 * CPU-time clocks stay at 1 ns. */
#include <stdio.h>
#include <time.h>

static long long ns(struct timespec t) { return t.tv_sec * 1000000000LL + t.tv_nsec; }

int main(void)
{
	int bad = 0;
	struct timespec res, a, b, d = { 0, 1000 };
	clock_getres(CLOCK_MONOTONIC, &res);
	long long worst = 0;
	for (int i = 0; i < 50; i++) {
		clock_gettime(CLOCK_MONOTONIC, &a);
		nanosleep(&d, NULL);
		clock_gettime(CLOCK_MONOTONIC, &b);
		long long over = ns(b) - ns(a) - 1000;
		if (over > worst) worst = over;
	}
	if (worst > 2 * ns(res) + 500000) {
		printf("a 1 us sleep overshot by %lld ns; CLOCK_MONOTONIC claims %lld ns resolution\n", worst, ns(res)); bad++;
	}
	clock_getres(CLOCK_PROCESS_CPUTIME_ID, &res);
	if (ns(res) != 1) { printf("CPU-time resolution %lld ns, want 1\n", ns(res)); bad++; }
	puts(bad ? "FAIL" : "OK");
	return bad != 0;
}
