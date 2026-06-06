/* Blocking syscalls must be interruptible by signals (Linux EINTR semantics):
 *  1. SIGKILL aborts a child blocked in read() on an empty pipe (child dies).
 *  2. SIGINT with an installed handler makes a blocked read() return -1/EINTR.
 *  3. SIGTERM (default action) kills a child blocked in read() — timeout-style.
 * Bounded by alarm() so a regression is a failure, not a suite hang. */
#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <string.h>
#include <sys/wait.h>

static volatile sig_atomic_t got_int;
static void on_int(int s) { (void)s; got_int = 1; }

static int kill_blocked_reader(int sig, int expect_sig)
{
	int p[2];
	if (pipe(p) != 0) return 0;
	pid_t pid = fork();
	if (pid == 0) {
		close(p[1]);
		char c;
		read(p[0], &c, 1);   /* blocks forever: no writer ever sends */
		_exit(0);
	}
	close(p[0]);
	usleep(50000);
	kill(pid, sig);
	int st = 0;
	if (waitpid(pid, &st, 0) != pid) return 0;
	close(p[1]);
	return WIFSIGNALED(st) && WTERMSIG(st) == expect_sig;
}

int main(void)
{
	alarm(20);   /* hard bound: a missed wakeup must time out, not hang */

	if (!kill_blocked_reader(SIGKILL, SIGKILL)) { puts("FAIL sigkill"); return 1; }
	if (!kill_blocked_reader(SIGTERM, SIGTERM)) { puts("FAIL sigterm"); return 1; }

	/* EINTR: a handled signal interrupts a blocked read and returns -1/EINTR. */
	int p[2];
	if (pipe(p) != 0) { puts("FAIL pipe"); return 1; }
	pid_t pid = fork();
	if (pid == 0) {
		struct sigaction sa;
		memset(&sa, 0, sizeof(sa));
		sa.sa_handler = on_int;   /* no SA_RESTART → read returns EINTR */
		sigaction(SIGINT, &sa, 0);
		close(p[1]);
		char c;
		errno = 0;
		ssize_t r = read(p[0], &c, 1);
		_exit(r < 0 && errno == EINTR && got_int ? 0 : 1);
	}
	close(p[0]);
	usleep(50000);
	kill(pid, SIGINT);
	int st = 0;
	waitpid(pid, &st, 0);
	close(p[1]);
	if (!(WIFEXITED(st) && WEXITSTATUS(st) == 0)) { puts("FAIL eintr"); return 1; }

	puts("OK");
	return 0;
}
