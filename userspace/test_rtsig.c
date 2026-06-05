/* RT signals queue (each instance delivered with its own si_value, lowest signo
 * first, FIFO within a signo) while a standard signal coalesces. */
#define _GNU_SOURCE
#include <stdio.h>
#include <signal.h>
#include <unistd.h>
#include <string.h>

#define N 5
static volatile int rt_count;
static int rt_vals[N];
static volatile int std_count;

static void rt_handler(int s, siginfo_t *si, void *uc)
{
	(void)s; (void)uc;
	if (rt_count < N) rt_vals[rt_count] = si->si_value.sival_int;
	rt_count++;
}

static void std_handler(int s) { (void)s; std_count++; }

int main(void)
{
	int rt = SIGRTMIN;

	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_sigaction = rt_handler;
	sa.sa_flags = SA_SIGINFO;
	sigaction(rt, &sa, 0);

	struct sigaction sb;
	memset(&sb, 0, sizeof(sb));
	sb.sa_handler = std_handler;
	sigaction(SIGUSR1, &sb, 0);

	sigset_t block, old;
	sigemptyset(&block);
	sigaddset(&block, rt);
	sigaddset(&block, SIGUSR1);
	sigprocmask(SIG_BLOCK, &block, &old);

	/* Queue N RT instances + several SIGUSR1 while blocked. */
	for (int i = 0; i < N; i++) {
		union sigval v; v.sival_int = 100 + i;
		sigqueue(getpid(), rt, v);
	}
	for (int i = 0; i < 4; i++) raise(SIGUSR1);

	sigprocmask(SIG_UNBLOCK, &block, 0);
	for (int spin = 0; spin < 1000000 && rt_count < N; spin++) usleep(100);

	if (rt_count != N) { printf("rtsig-fail count=%d\n", rt_count); return 0; }
	for (int i = 0; i < N; i++)
		if (rt_vals[i] != 100 + i) { printf("rtsig-fail order[%d]=%d\n", i, rt_vals[i]); return 0; }
	if (std_count != 1) { printf("rtsig-fail std=%d\n", std_count); return 0; }

	printf("OK\n");
	return 0;
}
