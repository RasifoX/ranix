/* sched_setaffinity actually pins: round-trip the mask, reject an empty mask,
 * and confirm a task restricted to CPU 0 only ever reports running on CPU 0. */
#define _GNU_SOURCE
#include <stdio.h>
#include <sched.h>
#include <unistd.h>
#include <errno.h>

int main(void)
{
	int ncpu = (int)sysconf(_SC_NPROCESSORS_ONLN);
	if (ncpu < 1) { printf("fail nproc\n"); return 0; }

	cpu_set_t set;

	/* empty mask → EINVAL */
	CPU_ZERO(&set);
	errno = 0;
	if (sched_setaffinity(0, sizeof(set), &set) != -1 || errno != EINVAL) {
		printf("fail empty errno=%d\n", errno); return 0;
	}

	/* pin to CPU 0 and read it back */
	CPU_ZERO(&set);
	CPU_SET(0, &set);
	if (sched_setaffinity(0, sizeof(set), &set) != 0) { printf("fail set0\n"); return 0; }

	cpu_set_t back;
	CPU_ZERO(&back);
	if (sched_getaffinity(0, sizeof(back), &back) != 0) { printf("fail get0\n"); return 0; }
	if (!CPU_ISSET(0, &back)) { printf("fail roundtrip\n"); return 0; }
	for (int i = 1; i < ncpu && i < 64; i++)
		if (CPU_ISSET(i, &back)) { printf("fail extra-cpu %d\n", i); return 0; }

	/* with only CPU 0 allowed, every observation must report CPU 0 */
	for (int i = 0; i < 2000; i++) {
		int c = sched_getcpu();
		if (c != 0) { printf("fail ran-on %d\n", c); return 0; }
		if ((i & 63) == 0) sched_yield();
	}

	/* restore full mask */
	CPU_ZERO(&set);
	for (int i = 0; i < ncpu && i < 64; i++) CPU_SET(i, &set);
	if (sched_setaffinity(0, sizeof(set), &set) != 0) { printf("fail restore\n"); return 0; }

	printf("OK\n");
	return 0;
}
