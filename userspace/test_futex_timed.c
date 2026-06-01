/* Timed futex path (pthread_cond_timedwait): a wait that must time out, and a
 * wait that must be woken before its deadline. Exercises the kernel's
 * event-driven timed futex (sleeping_on_queue + wake_deadline deadline sweep).
 * No wall-clock duration assert: the futex deadline runs on the scheduler tick
 * while clock_gettime runs on the high-res timer, and the two diverge under
 * emulation. The wake case proves the wait actually blocks (the signaller fires
 * after 50ms, so a non-blocking wait would time out instead of returning 0). */
#include <stdio.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>

static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  c = PTHREAD_COND_INITIALIZER;

static void abstime(struct timespec *ts, long add_ms)
{
	clock_gettime(CLOCK_REALTIME, ts);
	ts->tv_sec  += add_ms / 1000;
	ts->tv_nsec += (add_ms % 1000) * 1000000L;
	if (ts->tv_nsec >= 1000000000L) { ts->tv_sec++; ts->tv_nsec -= 1000000000L; }
}

static void *signaller(void *arg)
{
	(void)arg;
	usleep(50000);
	pthread_mutex_lock(&m);
	pthread_cond_signal(&c);
	pthread_mutex_unlock(&m);
	return NULL;
}

int main(void)
{
	struct timespec ts;

	pthread_mutex_lock(&m);
	abstime(&ts, 200);
	int rc = pthread_cond_timedwait(&c, &m, &ts);
	pthread_mutex_unlock(&m);
	if (rc != ETIMEDOUT) { printf("FAIL timeout rc=%d\n", rc); return 1; }

	pthread_t th;
	pthread_create(&th, NULL, signaller, NULL);
	pthread_mutex_lock(&m);
	abstime(&ts, 5000);
	rc = pthread_cond_timedwait(&c, &m, &ts);
	pthread_mutex_unlock(&m);
	pthread_join(th, NULL);
	if (rc != 0) { printf("FAIL wake rc=%d\n", rc); return 1; }

	puts("OK");
	return 0;
}
