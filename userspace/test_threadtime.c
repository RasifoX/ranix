/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* A process's CPU time includes the threads that have finished: `time` and wait4
 * report it, and a program doing its work in threads must not show zero. */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static volatile long sink;

static void *spin(void *a)
{
	struct timespec s, n;
	clock_gettime(CLOCK_MONOTONIC, &s);
	do {
		for (int i = 0; i < 100000; i++) sink += i;
		clock_gettime(CLOCK_MONOTONIC, &n);
	} while ((n.tv_sec - s.tv_sec) * 1000 + (n.tv_nsec - s.tv_nsec) / 1000000 < 300);
	return a;
}

int main(void)
{
	pid_t c = fork();
	if (c == 0) {
		pthread_t t;
		pthread_create(&t, NULL, spin, NULL);
		pthread_join(t, NULL);
		_exit(0);
	}
	struct rusage ru;
	int st;
	if (wait4(c, &st, 0, &ru) != c) { puts("FAIL wait4"); return 1; }
	long ms = ru.ru_utime.tv_sec * 1000 + ru.ru_utime.tv_usec / 1000 +
	          ru.ru_stime.tv_sec * 1000 + ru.ru_stime.tv_usec / 1000;
	if (ms < 100) { printf("FAIL cpu %ld ms\n", ms); return 1; }
	puts("OK");
	return 0;
}
