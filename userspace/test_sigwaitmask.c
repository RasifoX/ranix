/* pselect and ppoll wait under the mask they are given, and sigsuspend's handler
 * runs under its own: a signal blocked everywhere else gets in there, the handler
 * runs, the call says EINTR and the old mask is back. GNU make's job control is
 * built on exactly this. */
#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <unistd.h>

static volatile sig_atomic_t got, seen_blocked;

static void on_chld(int s)
{
	sigset_t cur;
	(void)s;
	sigprocmask(SIG_BLOCK, NULL, &cur);
	seen_blocked = sigismember(&cur, SIGUSR1);
	got++;
}

static int kid(void)
{
	pid_t k = fork();
	if (k == 0) _exit(0);
	return k > 0;
}

static int blocked_again(void)
{
	sigset_t cur;
	sigprocmask(SIG_BLOCK, NULL, &cur);
	return sigismember(&cur, SIGCHLD) == 1;
}

int main(void)
{
	sigset_t block, wait_with;
	int p[2], bad = 0;

	if (pipe(p) != 0) { puts("FAIL pipe"); return 1; }
	sigemptyset(&block);
	sigaddset(&block, SIGCHLD);
	sigprocmask(SIG_BLOCK, &block, NULL);
	signal(SIGCHLD, on_chld);
	sigemptyset(&wait_with);
	sigaddset(&wait_with, SIGUSR1);

	fd_set r;
	FD_ZERO(&r);
	FD_SET(p[0], &r);
	got = 0;
	if (!kid()) bad |= 1;
	usleep(20000);                     /* the signal is pending before the wait */
	struct timespec ts = { 5, 0 };
	int n = pselect(p[0] + 1, &r, NULL, NULL, &ts, &wait_with);
	if (n != -1 || errno != EINTR || got != 1) { printf("pselect n=%d e=%d got=%d\n", n, errno, (int)got); bad |= 2; }
	if (seen_blocked != 1) bad |= 4;   /* handler ran under the given mask */
	if (!blocked_again()) bad |= 8;
	while (waitpid(-1, NULL, 0) > 0) ;

	struct pollfd pf = { .fd = p[0], .events = POLLIN };
	got = 0;
	if (!kid()) bad |= 1;
	usleep(20000);
	n = ppoll(&pf, 1, &ts, &wait_with);
	if (n != -1 || errno != EINTR || got != 1) { printf("ppoll n=%d e=%d got=%d\n", n, errno, (int)got); bad |= 16; }
	if (!blocked_again()) bad |= 32;
	while (waitpid(-1, NULL, 0) > 0) ;

	got = 0;
	if (!kid()) bad |= 1;
	usleep(20000);
	sigsuspend(&wait_with);
	if (got != 1 || seen_blocked != 1) { printf("sigsuspend got=%d\n", (int)got); bad |= 64; }
	if (!blocked_again()) bad |= 128;
	while (waitpid(-1, NULL, 0) > 0) ;

	/* the signal comes while the wait sleeps, the case make actually hits */
	got = 0;
	pid_t k = fork();
	if (k == 0) { usleep(200000); _exit(0); }
	FD_ZERO(&r);
	FD_SET(p[0], &r);
	n = pselect(p[0] + 1, &r, NULL, NULL, &ts, &wait_with);
	if (n != -1 || errno != EINTR || got != 1) { printf("sleeping n=%d e=%d got=%d\n", n, errno, (int)got); bad |= 1024; }
	if (!blocked_again()) bad |= 2048;
	while (waitpid(-1, NULL, 0) > 0) ;

	/* no signal: the mask is back after a plain timeout too */
	sigset_t none;
	sigemptyset(&none);
	struct timespec shortt = { 0, 10000000 };
	FD_ZERO(&r);
	FD_SET(p[0], &r);
	if (pselect(p[0] + 1, &r, NULL, NULL, &shortt, &none) != 0) bad |= 256;
	if (!blocked_again()) bad |= 512;

	if (bad) printf("FAIL %d\n", bad); else puts("OK");
	return bad != 0;
}
