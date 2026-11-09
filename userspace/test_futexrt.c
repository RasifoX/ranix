/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* FUTEX_WAIT_BITSET with FUTEX_CLOCK_REALTIME times out at its wall-clock instant.
 *
 * The deadline used to be read on the boot clock, so a 2026 wall-clock instant
 * lay decades ahead and the wait never timed out. glibc's
 * pthread_cond_timedwait waits this way: Claude Code (Bun) threads slept until
 * some unrelated wake-up, and its first start took ~97 s instead of ~1 s. */
#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <sys/syscall.h>

#define FUTEX_WAIT_BITSET    9
#define FUTEX_PRIVATE_FLAG   128
#define FUTEX_CLOCK_REALTIME 256

static double mono_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }

static int wait_until(clockid_t clk, int flags, long ms)
{
	static uint32_t word = 0;
	struct timespec d; clock_gettime(clk, &d);
	d.tv_nsec += ms * 1000000L; d.tv_sec += d.tv_nsec / 1000000000L; d.tv_nsec %= 1000000000L;
	double t0 = mono_ms();
	long r = syscall(SYS_futex, &word, FUTEX_WAIT_BITSET | FUTEX_PRIVATE_FLAG | flags, 0, &d, NULL, 0xffffffffu);
	int e = errno; double dt = mono_ms() - t0;
	if (r != -1 || e != ETIMEDOUT || dt < ms - 5 || dt > ms + 400) {
		printf("FAIL %s: r=%ld errno=%d after %.0f ms (want ETIMEDOUT after %ld ms)\n",
		       flags ? "CLOCK_REALTIME" : "monotonic", r, e, dt, ms);
		return 1;
	}
	return 0;
}

int main(void)
{
	struct timespec rt; clock_gettime(CLOCK_REALTIME, &rt);
	if (rt.tv_sec < 1000000000L) printf("note: wall clock not set (%ld)\n", (long)rt.tv_sec);
	int f = 0;
	f |= wait_until(CLOCK_MONOTONIC, 0, 100);
	f |= wait_until(CLOCK_REALTIME, FUTEX_CLOCK_REALTIME, 100);
	f |= wait_until(CLOCK_REALTIME, FUTEX_CLOCK_REALTIME, 300);
	if (!f) printf("OK\n");
	return f;
}
