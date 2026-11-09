/* rename that walks through directories the dentry cache does not keep (procfs
 * pid directories) gives back what the walks made: thousands of renames that
 * fail leave kernel memory where it was.
 * - rename("/proc/A/nosuch", "/proc/B/nosuch"): both parents uncached;
 * - rename("/proc/A/nosuch", "/tmp/..."): the first one only (EXDEV).
 * Each case runs 3 rounds of 20000 calls after a warm-up; MemFree must not drop
 * by more than 2 MiB in every round (a leak of one inode per call is well over
 * 4 MiB a round, steadily). Also checks the calls fail as on Linux.
 * Prints OK, or FAIL lines. */
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define ROUND 20000

static long memfree_kib(void)
{
	FILE *f = fopen("/proc/meminfo", "r");
	if (!f) return -1;
	char line[128];
	long v = -1;
	while (fgets(line, sizeof(line), f))
		if (sscanf(line, "MemFree: %ld", &v) == 1) break;
	fclose(f);
	return v;
}

static int fails;

static void rounds(const char *what, const char *from, const char *to)
{
	for (int i = 0; i < 2000; i++) rename(from, to);
	errno = 0;
	if (rename(from, to) == 0) { printf("FAIL %s: rename reported success\n", what); fails++; }
	int leaked_every_round = 1;
	long drop[3];
	for (int r = 0; r < 3; r++) {
		long before = memfree_kib();
		for (int i = 0; i < ROUND; i++) rename(from, to);
		long after = memfree_kib();
		drop[r] = before - after;
		if (drop[r] <= 2048) leaked_every_round = 0;
	}
	if (leaked_every_round) {
		printf("FAIL %s: MemFree fell %ld, %ld, %ld KiB over 3 x %d renames\n",
		       what, drop[0], drop[1], drop[2], ROUND);
		fails++;
	}
}

int main(void)
{
	pid_t c = fork();
	if (c == 0) { for (;;) pause(); }
	char a[64], b[64];
	snprintf(a, sizeof(a), "/proc/%d/nosuch", (int)getpid());
	snprintf(b, sizeof(b), "/proc/%d/nosuch", (int)c);
	if (memfree_kib() < 0) { printf("FAIL no MemFree in /proc/meminfo\n"); fails++; }
	rounds("proc -> proc", a, b);
	rounds("proc -> tmp", a, "/tmp/rename-vleak-target");
	kill(c, SIGKILL);
	waitpid(c, NULL, 0);
	if (fails) return 1;
	printf("OK\n");
	return 0;
}
