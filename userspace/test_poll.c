/* poll() and select() blocking + timeout on a pipe: a timed poll on an empty
 * pipe returns 0, a blocking poll/select wakes when a child writes. Exercises
 * the kernel's fd readiness wait path. */
#include <stdio.h>
#include <poll.h>
#include <unistd.h>
#include <sys/select.h>
#include <sys/wait.h>

int main(void)
{
	int fd[2];
	if (pipe(fd) < 0) { puts("FAIL pipe"); return 1; }

	struct pollfd pf = { .fd = fd[0], .events = POLLIN, .revents = 0 };
	if (poll(&pf, 1, 100) != 0) { puts("FAIL poll-timeout"); return 1; }

	pid_t pid = fork();
	if (pid == 0) { usleep(50000); write(fd[1], "x", 1); _exit(0); }
	pf.revents = 0;
	int r = poll(&pf, 1, 5000);
	if (r != 1 || !(pf.revents & POLLIN)) { printf("FAIL poll r=%d rev=%x\n", r, pf.revents); return 1; }
	char c; read(fd[0], &c, 1);
	waitpid(pid, NULL, 0);

	int fd2[2];
	if (pipe(fd2) < 0) { puts("FAIL pipe2"); return 1; }
	pid_t pid2 = fork();
	if (pid2 == 0) { usleep(50000); write(fd2[1], "y", 1); _exit(0); }
	fd_set rs; FD_ZERO(&rs); FD_SET(fd2[0], &rs);
	r = select(fd2[0] + 1, &rs, NULL, NULL, NULL);
	if (r != 1 || !FD_ISSET(fd2[0], &rs)) { printf("FAIL select r=%d\n", r); return 1; }
	waitpid(pid2, NULL, 0);

	puts("OK");
	return 0;
}
