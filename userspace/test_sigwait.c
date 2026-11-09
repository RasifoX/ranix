/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* sigwait, sigwaitinfo and sigtimedwait take a signal without a handler: the usual
 * way a threaded daemon handles signals, and how libc runs timer callbacks. */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define WANT(c) do { if (!(c)) { printf("FAIL line %d errno=%d\n", __LINE__, errno); return 1; } } while (0)

static void *late(void *a)
{
	usleep(100000);
	kill(getpid(), SIGUSR1);
	return a;
}

int main(void)
{
	sigset_t set;
	sigemptyset(&set);
	sigaddset(&set, SIGUSR1); sigaddset(&set, SIGRTMIN + 2); sigaddset(&set, SIGCHLD);
	pthread_sigmask(SIG_BLOCK, &set, NULL);

	int s = 0;
	raise(SIGUSR1);
	WANT(sigwait(&set, &s) == 0 && s == SIGUSR1);

	struct timespec to = {0, 50000000};
	WANT(sigtimedwait(&set, NULL, &to) == -1 && errno == EAGAIN);
	to.tv_nsec = 0;
	WANT(sigtimedwait(&set, NULL, &to) == -1 && errno == EAGAIN);

	siginfo_t si;
	union sigval v = { .sival_int = 55 };
	WANT(sigqueue(getpid(), SIGRTMIN + 2, v) == 0);
	WANT(sigwaitinfo(&set, &si) == SIGRTMIN + 2 && si.si_value.sival_int == 55 && si.si_code == SI_QUEUE);

	/* asleep when it arrives, sent by another thread */
	pthread_t t;
	pthread_create(&t, NULL, late, NULL);
	to.tv_sec = 5;
	WANT(sigtimedwait(&set, &si, &to) == SIGUSR1);
	pthread_join(t, NULL);

	/* SIGCHLD's default action is to ignore it, but a waiter still gets it */
	pid_t c = fork();
	if (c == 0) _exit(0);
	WANT(sigtimedwait(&set, &si, &to) == SIGCHLD);
	waitpid(c, NULL, 0);

	/* the mask is what it was: still blocked, so this stays pending */
	sigset_t pend;
	raise(SIGUSR1);
	sigpending(&pend);
	WANT(sigismember(&pend, SIGUSR1));
	puts("OK");
	return 0;
}
