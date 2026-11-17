/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* Giving back a large heap from a threaded process. Each page unmapped used to
 * shoot down the other CPUs on its own: with a second thread alive, 1 GB of heap
 * was a quarter of a million IPI round trips in one syscall, seconds without a
 * yield, and SIGKILL waited for all of it (found by test_fuzz: 15 s, the
 * softlockup watchdog and an unkillable task). The shrink must be quick and a
 * kill during a large grow must land quickly. Prints OK or FAIL lines. */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <sys/syscall.h>
#include <unistd.h>

/* musl's sbrk takes only 0: move the break with the system call itself */
static void *xsbrk(intptr_t n)
{
	char *cur = (char *)syscall(SYS_brk, 0);
	if (!n) return cur;
	char *want = cur + n;
	if ((char *)syscall(SYS_brk, want) != want) { errno = ENOMEM; return (void *)-1; }
	return cur;
}

static int xbrk(void *p)
{
	return (void *)syscall(SYS_brk, p) == p ? 0 : -1;
}

static unsigned long SIZE = 1024UL << 20;   /* halved until it fits */

static double now(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

static void *spin(void *a)
{
	(void)a;
	for (;;) { volatile int x = 0; for (int i = 0; i < 1000000; i++) x += i; usleep(100); }
	return NULL;
}

int main(void)
{
	int bad = 0;
	char *base = xsbrk(0);
	while (xsbrk((intptr_t)SIZE) == (void *)-1) {
		if (SIZE <= (64UL << 20)) {
			printf("SKIP: sbrk %lu MB: %s\nOK\n", SIZE >> 20, strerror(errno));
			return 0;
		}
		SIZE /= 2;
	}
	for (unsigned long o = 0; o < SIZE; o += 4096) base[o] = 1;
	/* the sibling comes after the heap grew: its stack may be mapped just past
	 * the break, where brk could not grow again */
	pthread_t t;
	pthread_create(&t, NULL, spin, NULL);   /* unmaps must now reach its CPU */
	double t0 = now();
	if (xbrk(base) != 0) { printf("FAIL: brk shrink: %s\n", strerror(errno)); bad++; }
	double shrink = now() - t0;
	printf("shrink %lu MB: %.3f s\n", SIZE >> 20, shrink);
	/* a batch per shootdown: well under 0.1 s per 256 MB; one per page: seconds */
	double limit = 0.5 * (double)(SIZE >> 20) / 256;
	if (shrink > limit) { printf("FAIL: shrink took %.3f s, limit %.3f\n", shrink, limit); bad++; }

	/* a kill in the middle of a large grow ends the syscall */
	pid_t c = fork();
	if (c == 0) {
		/* the room above the break was taken back above; keep it ours */
		char *b = xsbrk(0);
		if (xsbrk((intptr_t)SIZE) == (void *)-1) _exit(1);
		pthread_t t2;
		pthread_create(&t2, NULL, spin, NULL);
		for (;;) {
			xbrk(b);
			if (xsbrk((intptr_t)SIZE) == (void *)-1) _exit(1);
		}
	}
	usleep(300000);
	t0 = now();
	kill(c, SIGKILL);
	waitpid(c, NULL, 0);
	double kl = now() - t0;
	printf("kill latency %.3f s\n", kl);
	if (kl > 1.0) { printf("FAIL: SIGKILL took %.3f s\n", kl); bad++; }

	if (!bad) printf("OK\n");
	return bad != 0;
}
