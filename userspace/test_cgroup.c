/* cgroups v2 pids controller: put ourselves in a cgroup with pids.max=4,
 * then fork until rejected. self + 3 children = 4, so the 4th fork must
 * fail with EAGAIN. */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>

int main(void)
{
	mkdir("/cgroup/test", 0755);

	int fd = open("/cgroup/test/pids.max", O_WRONLY);
	if (fd < 0) { printf("cgroup-fail open\n"); return 0; }
	write(fd, "4\n", 2);
	close(fd);

	fd = open("/cgroup/test/cgroup.procs", O_WRONLY);
	if (fd < 0) { printf("cgroup-fail procs\n"); return 0; }
	char b[16]; int n = sprintf(b, "%d\n", (int)getpid());
	write(fd, b, n);
	close(fd);

	pid_t kids[16]; int nk = 0, eagain = 0;
	for (int i = 0; i < 10; i++) {
		pid_t p = fork();
		if (p == 0) { sleep(30); _exit(0); }   /* hold the count */
		if (p < 0) { eagain = 1; break; }
		kids[nk++] = p;
	}
	for (int i = 0; i < nk; i++) { kill(kids[i], SIGKILL); waitpid(kids[i], NULL, 0); }

	printf("%s\n", (eagain && nk == 3) ? "cgroup-ok" : "cgroup-fail");
	return 0;
}
