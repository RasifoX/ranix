/* SMP lost-wakeup hunt: hammer the primitives glibc/liblzma condvars sit on.
 * Two phases, both bounded so a lost wakeup hangs (caught by the test timeout)
 * rather than silently passing.
 *
 * Phase 1 — pthread condvar producer/consumer. N consumers wait on a condvar;
 * a producer enqueues items and signals in a tight loop with no delay, hitting
 * the wake-before-wait window (signal landing between a consumer's predicate
 * check and its FUTEX_WAIT enqueue). Every produced item must be consumed; if a
 * wakeup is lost a consumer sleeps forever and the join hangs.
 *
 * Phase 2 — raw FUTEX_WAIT/FUTEX_WAKE mirroring the condvar internal protocol:
 * waiter reads the word, a waker bumps it and FUTEX_WAKEs in a tight race. The
 * waiter must never block past a bumped word.
 *
 * Spawns CPU-hog children so all phases run under real 4-CPU contention. */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/syscall.h>

#define FUTEX_WAIT 0
#define FUTEX_WAKE 1

#define NCONS   3
#define NITEMS  4000

static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  cv  = PTHREAD_COND_INITIALIZER;
static int queue;
static int done;
static long consumed[NCONS];

static void *consumer(void *arg)
{
	long id = (long)arg;
	for (;;) {
		pthread_mutex_lock(&mtx);
		while (queue == 0 && !done)
			pthread_cond_wait(&cv, &mtx);
		if (queue == 0 && done) { pthread_mutex_unlock(&mtx); break; }
		queue--;
		consumed[id]++;
		pthread_mutex_unlock(&mtx);
	}
	return NULL;
}

static int phase_condvar(void)
{
	pthread_t th[NCONS];
	for (long i = 0; i < NCONS; i++)
		if (pthread_create(&th[i], NULL, consumer, (void *)i) != 0) return 1;

	for (int i = 0; i < NITEMS; i++) {
		pthread_mutex_lock(&mtx);
		queue++;
		pthread_cond_signal(&cv);
		pthread_mutex_unlock(&mtx);
	}
	pthread_mutex_lock(&mtx);
	done = 1;
	pthread_cond_broadcast(&cv);
	pthread_mutex_unlock(&mtx);

	for (int i = 0; i < NCONS; i++) pthread_join(th[i], NULL);

	long total = 0;
	for (int i = 0; i < NCONS; i++) total += consumed[i];
	return total == NITEMS ? 0 : 1;
}

static int fx;        /* raw futex word */
static volatile int fx_stop;

static int futex(int *u, int op, int val)
{
	return syscall(SYS_futex, u, op, val, NULL, NULL, 0);
}

static void *fx_waker(void *arg)
{
	(void)arg;
	while (!fx_stop) {
		__atomic_add_fetch(&fx, 1, __ATOMIC_SEQ_CST);
		futex(&fx, FUTEX_WAKE, 1);
	}
	return NULL;
}

static int phase_rawfutex(void)
{
	pthread_t w;
	fx_stop = 0;
	if (pthread_create(&w, NULL, fx_waker, NULL) != 0) return 1;
	for (int i = 0; i < 30000; i++) {
		int v = __atomic_load_n(&fx, __ATOMIC_SEQ_CST);
		/* Wait only while the word is still v. If the waker already bumped it
		 * the kernel must return EAGAIN, not block — a missed bump here would
		 * hang past fx_stop. */
		futex(&fx, FUTEX_WAIT, v);
	}
	fx_stop = 1;
	__atomic_add_fetch(&fx, 1, __ATOMIC_SEQ_CST);
	futex(&fx, FUTEX_WAKE, 1);
	pthread_join(w, NULL);
	return 0;
}

int main(void)
{
	pid_t hog[3];
	for (int i = 0; i < 3; i++) {
		hog[i] = fork();
		if (hog[i] == 0) {
			volatile unsigned long x = 0;
			for (;;) x++;
		}
	}

	int rc = phase_condvar();
	if (rc == 0) rc = phase_rawfutex();

	for (int i = 0; i < 3; i++) { kill(hog[i], SIGKILL); waitpid(hog[i], NULL, 0); }

	printf(rc == 0 ? "OK\n" : "FAIL\n");
	return rc;
}
