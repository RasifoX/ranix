/* Process-directed signals (kill(2)) go to a thread that hasn't blocked them,
 * not blindly to the group leader: main blocks SIGUSR1, the worker unblocks it,
 * and kill(getpid(),SIGUSR1) must be handled by the worker. */
#define _GNU_SOURCE
#include <stdio.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>

static volatile int got;
static void handler(int s) { (void)s; got = 1; }

static volatile int ready;
static void *worker(void *a)
{
	(void)a;
	sigset_t set;
	sigemptyset(&set);
	sigaddset(&set, SIGUSR1);
	pthread_sigmask(SIG_UNBLOCK, &set, 0);   /* only this thread accepts SIGUSR1 */
	ready = 1;
	while (!got) pause();
	return 0;
}

int main(void)
{
	signal(SIGUSR1, handler);
	sigset_t set;
	sigemptyset(&set);
	sigaddset(&set, SIGUSR1);
	pthread_sigmask(SIG_BLOCK, &set, 0);     /* main refuses SIGUSR1 */

	pthread_t th;
	pthread_create(&th, 0, worker, 0);       /* inherits main's mask: SIGUSR1 blocked */
	while (!ready) usleep(1000);

	if (kill(getpid(), SIGUSR1) != 0) { printf("killgroup-fail send\n"); return 0; }
	pthread_join(th, 0);
	if (!got) { printf("killgroup-fail nodeliver\n"); return 0; }

	printf("OK\n");
	return 0;
}
