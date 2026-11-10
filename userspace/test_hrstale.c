/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* A sleep with a far end woken early, again and again, must not use up the
 * fine timer. With a POSIX timer 30 s off, the kernel's timer thread sleeps
 * towards that end and is woken early by every short timerfd; 400 of them,
 * then the median overshoot of 300 us nanosleeps stays under 200 us (Ranix
 * kept every early-woken end in a 64-slot per-CPU queue until it came due:
 * full, sleeps ended on the tick, ~1.6 ms late). */
#define _GNU_SOURCE
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#define N 100

static long long now_ns(void)
{
	struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000000000LL + t.tv_nsec;
}

static int cmp(const void *a, const void *b)
{
	long long x = *(const long long *)a, y = *(const long long *)b;
	return x < y ? -1 : x > y;
}

int main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	struct sigevent sev = { .sigev_notify = SIGEV_NONE };
	timer_t far;
	struct itimerspec f30 = { .it_value = { 30, 0 } };
	if (timer_create(CLOCK_MONOTONIC, &sev, &far) || timer_settime(far, 0, &f30, NULL)) {
		puts("timer_create failed"); puts("FAIL"); return 1;
	}
	int fd = timerfd_create(CLOCK_MONOTONIC, 0);
	struct itimerspec s50 = { .it_value = { 0, 50000 } };
	for (int i = 0; i < 400; i++) {
		uint64_t e;
		if (timerfd_settime(fd, 0, &s50, NULL) || read(fd, &e, sizeof(e)) != sizeof(e)) {
			puts("timerfd failed"); puts("FAIL"); return 1;
		}
	}
	static long long over[N];
	struct timespec d = { 0, 300000 };
	for (int i = 0; i < N; i++) {
		long long a = now_ns();
		nanosleep(&d, NULL);
		over[i] = now_ns() - a - 300000;
	}
	qsort(over, N, sizeof(over[0]), cmp);
	printf("after 400 short timerfds under a 30 s timer: 300 us sleep median overshoot %lld us, max %lld us\n",
	       over[N / 2] / 1000, over[N - 1] / 1000);
	if (over[N / 2] >= 200000) { puts("FAIL"); return 1; }
	puts("OK");
	return 0;
}
