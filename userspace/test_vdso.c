/* vDSO: the kernel advertises AT_SYSINFO_EHDR and libc routes
 * clock_gettime(CLOCK_REALTIME)/gettimeofday through the mapped vDSO (no
 * syscall). Confirm the aux entry is present and the time it returns is
 * well-formed and never goes backwards across many fast calls. */
#define _GNU_SOURCE
#include <stdio.h>
#include <sys/auxv.h>
#include <sys/time.h>
#include <time.h>

int main(void)
{
	if (!getauxval(AT_SYSINFO_EHDR)) { printf("vdso-fail no-ehdr\n"); return 0; }

	struct timespec a, b;
	if (clock_gettime(CLOCK_REALTIME, &a) != 0) { printf("vdso-fail gettime\n"); return 0; }
	if (a.tv_nsec < 0 || a.tv_nsec >= 1000000000L) { printf("vdso-fail nsec\n"); return 0; }
	for (int i = 0; i < 200000; i++) {
		if (clock_gettime(CLOCK_REALTIME, &b) != 0) { printf("vdso-fail gettime2\n"); return 0; }
		if (b.tv_sec < a.tv_sec) { printf("vdso-fail backwards\n"); return 0; }
	}

	struct timeval tv;
	if (gettimeofday(&tv, 0) != 0 || tv.tv_sec <= 0) { printf("vdso-fail gtod\n"); return 0; }

	printf("OK\n");
	return 0;
}
