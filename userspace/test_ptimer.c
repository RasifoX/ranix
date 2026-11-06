/* timer_create: one-shot and periodic signals with their value, timer_gettime, deletion, and the SIGEV_THREAD form libc builds on top.
 * Unimplemented, it was the most frequent syscall in Ranix's own log. */
#define _GNU_SOURCE
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static volatile int hits, val, code;
static volatile long on;
static sem_t done;

static void h(int s, siginfo_t *si, void *u)
{
	(void)s; (void)u;
	hits++; val = si->si_value.sival_int; code = si->si_code;
	on = syscall(SYS_gettid);
}

static void th(union sigval v) { val = v.sival_int; sem_post(&done); }

#define WANT(c) do { if (!(c)) { printf("FAIL line %d hits=%d val=%d\n", __LINE__, hits, val); return 1; } } while (0)

static void msleep(int ms)
{
	struct timespec t = { ms / 1000, ms % 1000 * 1000000L };
	while (nanosleep(&t, &t)) ;
}

int main(void)
{
	struct sigaction sa;
	memset(&sa, 0, sizeof sa);
	sa.sa_sigaction = h;
	sa.sa_flags = SA_SIGINFO | SA_RESTART;
	sigaction(SIGRTMIN + 1, &sa, NULL);

	struct sigevent ev;
	memset(&ev, 0, sizeof ev);
	ev.sigev_notify = SIGEV_SIGNAL;
	ev.sigev_signo = SIGRTMIN + 1;
	ev.sigev_value.sival_int = 77;
	timer_t t;
	WANT(timer_create(CLOCK_MONOTONIC, &ev, &t) == 0);

	struct itimerspec one = { {0, 0}, {0, 20000000} }, cur;
	WANT(timer_settime(t, 0, &one, NULL) == 0);
	msleep(300);
	WANT(hits == 1 && val == 77 && code == SI_TIMER);
	WANT(timer_gettime(t, &cur) == 0 && !cur.it_value.tv_sec && !cur.it_value.tv_nsec);

	hits = 0;
	struct itimerspec per = { {0, 20000000}, {0, 20000000} };
	WANT(timer_settime(t, 0, &per, NULL) == 0);
	WANT(timer_gettime(t, &cur) == 0 && cur.it_interval.tv_nsec == 20000000);
	msleep(500);
	WANT(hits >= 3);
	WANT(timer_delete(t) == 0);
	int h0 = hits;
	msleep(150);
	WANT(hits <= h0 + 1);
	WANT(timer_gettime(t, &cur) == -1);

	/* absolute time on the wall clock */
	hits = 0;
	WANT(timer_create(CLOCK_REALTIME, &ev, &t) == 0);
	struct itimerspec ab = {{0, 0}, {0, 0}};
	clock_gettime(CLOCK_REALTIME, &ab.it_value);
	ab.it_value.tv_nsec += 30000000;
	if (ab.it_value.tv_nsec >= 1000000000) { ab.it_value.tv_sec++; ab.it_value.tv_nsec -= 1000000000; }
	WANT(timer_settime(t, TIMER_ABSTIME, &ab, NULL) == 0);
	msleep(300);
	WANT(hits == 1);
	timer_delete(t);

	/* libc's SIGEV_THREAD */
	sem_init(&done, 0, 0);
	memset(&ev, 0, sizeof ev);
	ev.sigev_notify = SIGEV_THREAD;
	ev.sigev_notify_function = th;
	ev.sigev_value.sival_int = 88;
	val = 0;
	WANT(timer_create(CLOCK_MONOTONIC, &ev, &t) == 0);
	WANT(timer_settime(t, 0, &one, NULL) == 0);
	struct timespec dl;
	clock_gettime(CLOCK_REALTIME, &dl);
	dl.tv_sec += 5;
	WANT(sem_timedwait(&done, &dl) == 0 && val == 88);
	timer_delete(t);
	puts("OK");
	return 0;
}
