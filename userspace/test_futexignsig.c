/*
 * A blocking futex (pthread_cond_wait) must NOT wake with EINTR for a pending
 * signal that is ignored (SIG_IGN) — Linux only interrupts a blocking syscall
 * for a signal that is actually delivered to a handler (or a fatal default).
 * liblzma's multithreaded xz decoder relies on this: dpkg ignores SIGPIPE, and
 * a pending-but-ignored SIGPIPE on a decode worker must not abort its cond wait
 * (which manifested as "xz compressed data is corrupt").
 *
 * The worker ignores SIGPIPE, makes a real SIGPIPE pending by writing to a pipe
 * whose read end is closed, then waits on a condvar that is never signalled for
 * a bounded time. If the wait returns before the timeout, the kernel woke it on
 * the ignored pending signal — the bug. A clean run blocks the full timeout.
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>

static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  cv;
static volatile int    woke_early;

static void *worker(void *arg)
{
    (void)arg;
    signal(SIGPIPE, SIG_IGN);

    int fds[2];
    if (pipe(fds) == 0) {
        close(fds[0]);
        char c = 'x';
        (void)!write(fds[1], &c, 1);   /* EPIPE + pending SIG_IGN'd SIGPIPE */
        close(fds[1]);
    }

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    ts.tv_sec += 1;   /* 1s timeout; never signalled */

    pthread_mutex_lock(&mtx);
    int rc = pthread_cond_timedwait(&cv, &mtx, &ts);
    pthread_mutex_unlock(&mtx);

    if (rc != ETIMEDOUT)
        woke_early = 1;   /* spurious wake (EINTR-driven) before the deadline */
    return NULL;
}

int main(void)
{
    pthread_condattr_t a;
    pthread_condattr_init(&a);
    pthread_condattr_setclock(&a, CLOCK_MONOTONIC);
    pthread_cond_init(&cv, &a);

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    pthread_t t;
    pthread_create(&t, NULL, worker, NULL);
    pthread_join(t, NULL);

    clock_gettime(CLOCK_MONOTONIC, &t1);
    long ms = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;

    if (woke_early || ms < 900) {
        write(1, "FAIL:woke-early\n", 16);
        return 1;
    }
    write(1, "OK\n", 3);
    return 0;
}
