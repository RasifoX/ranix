/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* SA_RESTART: a handler returning leaves the interrupted read waiting, as if no
 * signal had come. Without the flag, or for a sleep, the call reports EINTR. */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define WANT(c) do { if (!(c)) { printf("FAIL line %d errno=%d hits=%d\n", __LINE__, errno, hits); return 1; } } while (0)

static volatile int hits;
static int pfd[2];

static void h(int s) { (void)s; hits++; }

static void *later(void *a)
{
	usleep(250000);
	write(pfd[1], "x", 1);
	return a;
}

static void on(int flags)
{
	struct sigaction sa;
	memset(&sa, 0, sizeof sa);
	sa.sa_handler = h;
	sa.sa_flags = flags;
	sigaction(SIGALRM, &sa, NULL);
	hits = 0;
	struct itimerval it = { {0, 0}, {0, 50000} };
	setitimer(ITIMER_REAL, &it, NULL);
}

int main(void)
{
	char c;
	pipe(pfd);
	pthread_t t;

	on(SA_RESTART);
	pthread_create(&t, NULL, later, NULL);
	WANT(read(pfd[0], &c, 1) == 1 && hits == 1);
	pthread_join(t, NULL);

	on(0);
	WANT(read(pfd[0], &c, 1) == -1 && errno == EINTR && hits == 1);

	on(SA_RESTART);
	struct timespec ts = {1, 0};
	WANT(nanosleep(&ts, NULL) == -1 && errno == EINTR && hits == 1);

	on(SA_RESTART);
	WANT(pause() == -1 && errno == EINTR);
	puts("OK");
	return 0;
}
