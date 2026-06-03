/* times(2): reports real per-process user/system CPU time (was all zeros).
 * Burning CPU in user mode grows tms_utime; a reaped child's time rolls up
 * into the parent's tms_cutime. */
#define _GNU_SOURCE
#include <stdio.h>
#include <sys/times.h>
#include <unistd.h>
#include <sys/wait.h>

static volatile unsigned long sink;
static void burn(int wall_ticks)
{
	clock_t start = times(0);
	while (times(0) - start < wall_ticks)
		for (volatile long i = 0; i < 500000; i++) sink += (unsigned long)i;
}

int main(void)
{
	struct tms a, b;
	times(&a);
	burn(25);                 /* ~250 ms of mostly user-mode arithmetic */
	times(&b);
	if (b.tms_utime <= a.tms_utime) { printf("times-fail utime %ld->%ld\n",
	                                          (long)a.tms_utime, (long)b.tms_utime); return 0; }

	pid_t c = fork();
	if (c == 0) { burn(20); _exit(0); }
	waitpid(c, 0, 0);

	struct tms d;
	times(&d);
	if (d.tms_cutime <= 0) { printf("times-fail cutime %ld\n", (long)d.tms_cutime); return 0; }

	printf("OK\n");
	return 0;
}
