/* ITIMER_PROF / ITIMER_VIRTUAL: arming a CPU-time interval timer delivers
 * SIGPROF / SIGVTALRM as the process burns CPU (previously these were silently
 * accepted and never fired). Invalid which is EINVAL. */
#define _GNU_SOURCE
#include <stdio.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>

static volatile sig_atomic_t prof_n, virt_n;
static void on_prof(int s) { (void)s; prof_n++; }
static void on_virt(int s) { (void)s; virt_n++; }

static void arm(int which, long usec)
{
	struct itimerval it;
	it.it_value.tv_sec = 0;    it.it_value.tv_usec = usec;
	it.it_interval.tv_sec = 0; it.it_interval.tv_usec = usec;
	setitimer(which, &it, 0);
}
static void disarm(int which) { struct itimerval z = {{0,0},{0,0}}; setitimer(which, &z, 0); }

static volatile unsigned long sink;

int main(void)
{
	signal(SIGPROF, on_prof);
	signal(SIGVTALRM, on_virt);

	/* ITIMER_PROF: user+sys CPU time */
	arm(2, 20000);   /* 20 ms */
	for (time_t t0 = time(0); prof_n < 2 && time(0) - t0 < 6; )
		for (volatile unsigned long i = 0; i < 3000000; i++) sink += i;
	disarm(2);
	if (prof_n < 2) { printf("itimer-fail prof n=%d\n", (int)prof_n); return 0; }

	/* ITIMER_VIRTUAL: user CPU time only */
	arm(1, 20000);
	for (time_t t0 = time(0); virt_n < 2 && time(0) - t0 < 6; )
		for (volatile unsigned long i = 0; i < 3000000; i++) sink += i;
	disarm(1);
	if (virt_n < 2) { printf("itimer-fail virt n=%d\n", (int)virt_n); return 0; }

	struct itimerval z = {{0,0},{0,0}};
	if (setitimer(99, &z, 0) != -1 || errno != EINVAL) { printf("itimer-fail einval\n"); return 0; }

	printf("OK\n");
	return 0;
}
