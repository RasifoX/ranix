/* pidfd_open + pidfd_send_signal: open a pidfd for a child and signal it through
 * the fd; the child must die from that signal. */
#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <sys/syscall.h>
#include <sys/wait.h>

int main(void)
{
	pid_t pid = fork();
	if (pid == 0) { for (;;) pause(); _exit(0); }

	usleep(50000);
	int pfd = (int)syscall(SYS_pidfd_open, pid, 0);
	if (pfd < 0) { puts("FAIL open"); return 1; }
	if (syscall(SYS_pidfd_send_signal, pfd, SIGTERM, (void *)0, 0u) < 0) {
		puts("FAIL signal"); return 1;
	}

	int st = 0;
	waitpid(pid, &st, 0);
	puts(WIFSIGNALED(st) && WTERMSIG(st) == SIGTERM ? "OK" : "FAIL");
	return 0;
}
