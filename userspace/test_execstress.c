/* Repro tool for the residual flake (pipe-fork-rw ~2%, "echo hello | cat" rarely
 * empty): unlike test_pipestress, this adds the missing ingredient — exec. Each
 * round forks a child that execs /bin/echo with stdout on a pipe, and the parent
 * reads it back, exercising the fork→exec→COW→pipe path. An empty/short read
 * means the exec'd writer's output was lost. Prints OK if every round delivered
 * "hello", else the failure count. */
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <sys/wait.h>

#define ROUNDS 300

int main(void)
{
	int fails = 0;
	for (int i = 0; i < ROUNDS; i++) {
		int fd[2];
		if (pipe(fd) < 0) { fails++; continue; }
		pid_t pid = fork();
		if (pid == 0) {
			close(fd[0]);
			dup2(fd[1], 1);
			close(fd[1]);
			execl("/bin/echo", "echo", "hello", (char *)0);
			_exit(127);
		}
		close(fd[1]);
		char buf[64];
		int got = 0;
		for (;;) {
			int n = (int)read(fd[0], buf + got, sizeof(buf) - 1 - got);
			if (n <= 0) break;
			got += n;
			if (got >= (int)sizeof(buf) - 1) break;
		}
		buf[got > 0 ? got : 0] = '\0';
		close(fd[0]);
		waitpid(pid, NULL, 0);
		if (got < 5 || !strstr(buf, "hello")) fails++;
	}
	printf(fails == 0 ? "OK\n" : "FAIL %d/%d\n", fails, ROUNDS);
	return 0;
}
