/* Repro tool for the chronic SMP residual: a tight fork+pipe+wait loop reliably
 * deadlocks (a woken reader/waiter that never gets scheduled — "PROC_READY stuck
 * on a runqueue"). 200 rounds: pipe → fork → child writes "hello" → parent reads
 * it → waitpid. Prints OK if all rounds complete, else hangs (the bug). NOT in
 * the gating suite — run /bin/test_pipestress by hand while hunting the race. */
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <sys/wait.h>

int main(void)
{
	int fails = 0;
	for (int i = 0; i < 200; i++) {
		int fd[2];
		if (pipe(fd) < 0) { fails++; continue; }
		pid_t pid = fork();
		if (pid == 0) {
			close(fd[0]);
			write(fd[1], "hello", 5);
			close(fd[1]);
			_exit(0);
		}
		close(fd[1]);
		char buf[16];
		int n = (int)read(fd[0], buf, sizeof(buf));
		close(fd[0]);
		waitpid(pid, NULL, 0);
		if (n != 5 || memcmp(buf, "hello", 5) != 0) fails++;
	}
	printf(fails == 0 ? "OK\n" : "FAIL %d/200\n", fails);
	return 0;
}
