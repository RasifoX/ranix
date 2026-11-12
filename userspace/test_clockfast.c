/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* Asking the time is cheap and never goes back: clock_gettime(CLOCK_MONOTONIC)
 * averages under 1 us a call over the best of 5 batches of 2000 (Linux: tens of
 * ns from the vDSO), and 100000 reads in a row never decrease.
 *
 * Under KVM without an invariant TSC flag (QEMU -cpu host), Ranix read the HPET:
 * a trap to the VMM's user space, 12 us a call on Casper, and 2.5 ms sleeps ended
 * 0.25-0.4 ms late (test_sleeptail). A normal OS uses kvm-clock there. */
#define _GNU_SOURCE
#include <stdio.h>
#include <time.h>

static long long ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000000000LL + t.tv_nsec; }

int main(void)
{
	long long best = -1, prev = ns();
	int back = 0;
	for (int b = 0; b < 5; b++) {
		long long a = ns();
		for (int i = 0; i < 2000; i++) ns();
		long long per = (ns() - a) / 2001;
		if (best < 0 || per < best) best = per;
	}
	for (int i = 0; i < 100000; i++) {
		long long t = ns();
		if (t < prev) back++;
		prev = t;
	}
	printf("clock_gettime %lld ns a call (best of 5), went back %d times\n", best, back);
	int bad = best >= 1000 || back;
	puts(bad ? "FAIL" : "OK");
	return bad;
}
