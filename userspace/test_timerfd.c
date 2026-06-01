/* timerfd: arm a 100ms one-shot, read() blocks until it fires and returns an
 * expiration count of 1. Exercises the kernel's timerfd wait + tick expiry. */
#include <stdio.h>
#include <sys/timerfd.h>
#include <unistd.h>
#include <stdint.h>
#include <string.h>

int main(void)
{
	int tfd = timerfd_create(CLOCK_MONOTONIC, 0);
	if (tfd < 0) { puts("FAIL create"); return 1; }

	struct itimerspec its;
	memset(&its, 0, sizeof(its));
	its.it_value.tv_nsec = 100000000;   /* 100ms one-shot */
	if (timerfd_settime(tfd, 0, &its, NULL) < 0) { puts("FAIL settime"); return 1; }

	uint64_t exp = 0;
	ssize_t n = read(tfd, &exp, sizeof(exp));   /* blocks until fire */
	if (n != 8 || exp < 1) { printf("FAIL read n=%zd exp=%llu\n", n, (unsigned long long)exp); return 1; }

	close(tfd);
	puts("OK");
	return 0;
}
