/* tkill and pthread_kill name one thread. If the kernel treats them as signals to
 * the process, a sibling that does not block the signal takes it and the sender,
 * blocking it until sigsuspend, sleeps forever. QEMU's coroutines do exactly this. */
#define _GNU_SOURCE
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <unistd.h>

static volatile int ran_on;

static void on_sig(int s) { (void)s; ran_on = (int)syscall(SYS_gettid); }

static void *th(void *a)
{
	(void)a;
	sigset_t all, wait;
	sigfillset(&all);
	pthread_sigmask(SIG_BLOCK, &all, NULL);
	pthread_kill(pthread_self(), SIGUSR2);
	sigfillset(&wait);
	sigdelset(&wait, SIGUSR2);
	sigdelset(&wait, SIGALRM);
	alarm(5);                     /* a wrong kernel leaves us here */
	if (!ran_on) sigsuspend(&wait);
	alarm(0);
	return (void *)(long)syscall(SYS_gettid);
}

static void bail(int s) { (void)s; puts("FAIL sender never got its signal"); _exit(1); }

int main(void)
{
	pthread_t t;
	void *tid;
	signal(SIGUSR2, on_sig);
	signal(SIGALRM, bail);
	if (pthread_create(&t, NULL, th, NULL)) { puts("FAIL create"); return 1; }
	pthread_join(t, &tid);
	if (ran_on != (int)(long)tid) { printf("FAIL ran on %d want %ld\n", ran_on, (long)tid); return 1; }
	puts("OK");
	return 0;
}
