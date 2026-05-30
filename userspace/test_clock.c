/*
 * test_clock — clock_gettime sanity + monotonicity regression test.
 *
 * Guards the high-resolution clock work: clock_gettime(CLOCK_MONOTONIC) must
 * return well-formed timespecs (nsec < 1e9), never go backwards, and actually
 * advance over a bounded CPU-bound interval. Prints "OK" or a "FAIL:" reason.
 */
#include <stdio.h>
#include <stdint.h>
#include <time.h>

static int64_t to_ns(struct timespec *t)
{
	return (int64_t)t->tv_sec * 1000000000LL + t->tv_nsec;
}

int main(void)
{
	struct timespec a, b;

	if (clock_gettime(CLOCK_MONOTONIC, &a) != 0) { printf("FAIL: gettime\n"); return 1; }
	if (a.tv_nsec < 0 || a.tv_nsec >= 1000000000L) { printf("FAIL: nsec-range\n"); return 1; }

	/* Spin until the clock advances (bounded). aarch64 advances at once via
	 * cntvct; a coarse tick clock advances within one tick. */
	volatile uint64_t spin = 0;
	int64_t na = to_ns(&a), nb = na;
	for (uint64_t i = 0; i < 2000000000ULL; i++) {
		spin += i;
		if ((i & 0xFFFFF) == 0) {
			if (clock_gettime(CLOCK_MONOTONIC, &b) != 0) { printf("FAIL: gettime2\n"); return 1; }
			nb = to_ns(&b);
			if (nb < na) { printf("FAIL: went-backwards\n"); return 1; }
			if (nb > na) break;
		}
	}

	if (nb <= na) { printf("FAIL: never-advanced\n"); return 1; }
	(void)spin;
	printf("OK\n");
	return 0;
}
