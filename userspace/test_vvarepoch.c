/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* Every vvar period continues the one before it: the clock the vDSO serves is
 * the period's base count and clock plus the count since, so a new period's
 * clock must be the old one's carried to the new base count, to the nanosecond
 * (the two floor()s of the same product differ by at most 1 ns).
 *
 * The tick read the base count and the clock with two TSC reads. Whatever came
 * between them stayed in the whole period: on the bare Casper an SMI (~16 us)
 * put a 10 ms period 16 us ahead once a minute or so, and a 2.5 ms sleep across
 * its end measured 7-14 us short (test_sleeptail 'woke early', 3 in 60000;
 * 0 under KVM, where only a few ns fall between the reads). This test sees the
 * gap at any size, so under KVM too: a period that does not continue the one
 * before by more than 1 ns fails. x86-64 with a calibrated counter only. */
#define _GNU_SOURCE
#include <stdio.h>
#include <time.h>
#include "../include/uapi/vdso.h"   /* struct vdso_vvar, VVAR_VADDR */

int main(void)
{
#if !defined(__x86_64__)
	puts("not x86-64: skipped");
	puts("OK");
	return 0;
#else
	const volatile struct vdso_vvar *v = (const volatile struct vdso_vvar *)VVAR_VADDR;
	struct timespec t0, t;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	if (!v->cnt_valid) { puts("no calibrated counter: skipped"); puts("OK"); return 0; }
	unsigned oseq = 0;
	unsigned long long ob = 0;
	long long om = 0, n = 0, bad = 0, jmin = 0, jmax = 0;
	for (;;) {
		clock_gettime(CLOCK_MONOTONIC, &t);
		if ((t.tv_sec - t0.tv_sec) * 1000000000LL + (t.tv_nsec - t0.tv_nsec) > 2000000000LL) break;
		unsigned q = v->seq;
		if ((q & 1) || q == oseq) continue;
		__asm__ volatile("" ::: "memory");
		unsigned long long b = v->cnt_base;
		long long m = v->mono_sec * 1000000000LL + v->mono_nsec;
		unsigned mult = v->cnt_mult, sh = v->cnt_shift;
		__asm__ volatile("" ::: "memory");
		if (v->seq != q) continue;
		if (oseq) {
			long long j = (m - om) - (long long)(((b - ob) * (unsigned long long)mult) >> sh);
			n++;
			if (j < jmin) jmin = j;
			if (j > jmax) jmax = j;
			if (j < 0 || j > 1) {
				if (bad < 5) printf("period %u -> %u: clock %lld ns off the one before\n", oseq, q, j);
				bad++;
			}
		}
		oseq = q; ob = b; om = m;
	}
	printf("%lld periods in 2 s, %lld off by more than 1 ns (min %lld ns, max %lld ns)\n", n, bad, jmin, jmax);
	puts(n < 20 ? "FAIL: too few vvar periods" : bad ? "FAIL" : "OK");
	return n < 20 || bad;
#endif
}
