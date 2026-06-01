/* time namespace: after unshare(CLONE_NEWTIME) and writing a +100s monotonic
 * offset, a child created in the new namespace sees CLOCK_MONOTONIC advanced
 * by ~100s relative to the parent (which stays in the old namespace). */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sched.h>
#include <time.h>
#include <sys/wait.h>

int main(void)
{
	if (unshare(CLONE_NEWTIME) != 0) { printf("timens-fail unshare\n"); return 0; }

	int fd = open("/proc/self/timens_offsets", O_WRONLY);
	if (fd < 0) { printf("timens-fail open\n"); return 0; }
	const char *off = "monotonic 100 0\n";
	if (write(fd, off, strlen(off)) < 0) { printf("timens-fail write\n"); close(fd); return 0; }
	close(fd);

	struct timespec parent_mono;
	clock_gettime(CLOCK_MONOTONIC, &parent_mono);

	pid_t pid = fork();
	if (pid == 0) {
		struct timespec child_mono;
		clock_gettime(CLOCK_MONOTONIC, &child_mono);
		long diff = child_mono.tv_sec - parent_mono.tv_sec;
		_exit((diff >= 95 && diff <= 115) ? 0 : 3);
	}

	int st;
	waitpid(pid, &st, 0);
	printf("%s\n", (WIFEXITED(st) && WEXITSTATUS(st) == 0) ? "timens-ok" : "timens-fail");
	return 0;
}
