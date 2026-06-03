/* times(2): reports real per-process user/system CPU time (was all zeros).
 * Burning CPU in user mode grows tms_utime; a reaped child's time rolls up
 * into the parent's tms_cutime. */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/times.h>
#include <sys/resource.h>
#include <unistd.h>
#include <sys/wait.h>

/* /proc/self/stat field 14 (utime). The comm field is parenthesised and may
 * contain spaces, so split after the final ')'. */
static long proc_self_utime(void)
{
	char buf[1024];
	int fd = open("/proc/self/stat", O_RDONLY);
	if (fd < 0) return -1;
	int n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0) return -1;
	buf[n] = '\0';
	char *p = strrchr(buf, ')');
	if (!p) return -1;
	int field = 3;                 /* token right after ')' is field 3 (state) */
	char *tok = strtok(p + 1, " ");
	while (tok && field < 14) { tok = strtok(NULL, " "); field++; }
	return tok ? atol(tok) : -1;
}

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

	/* same data via /proc/self/stat field 14 */
	if (proc_self_utime() <= 0) { printf("times-fail procstat utime=%ld\n", proc_self_utime()); return 0; }

	/* getrusage(RUSAGE_SELF) reports the same user time */
	struct rusage ru;
	getrusage(RUSAGE_SELF, &ru);
	if (ru.ru_utime.tv_sec == 0 && ru.ru_utime.tv_usec == 0) { printf("times-fail rusage self\n"); return 0; }

	pid_t c = fork();
	if (c == 0) { burn(20); _exit(0); }
	waitpid(c, 0, 0);

	struct tms d;
	times(&d);
	if (d.tms_cutime <= 0) { printf("times-fail cutime %ld\n", (long)d.tms_cutime); return 0; }

	struct rusage rc;
	getrusage(RUSAGE_CHILDREN, &rc);
	if (rc.ru_utime.tv_sec == 0 && rc.ru_utime.tv_usec == 0) { printf("times-fail rusage children\n"); return 0; }

	printf("OK\n");
	return 0;
}
