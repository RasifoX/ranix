/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* Timers fire when set: a 1 ms timerfd (read, and epoll on it), a timerfd with
 * TFD_TIMER_ABSTIME on CLOCK_MONOTONIC, and a POSIX timer signalling SIGRTMIN:
 * never early, median lateness under 400 us (LTP allows 400 us for a context switch). */
#define _GNU_SOURCE
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

static long long ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000000000LL + t.tv_nsec; }
static int cmp(const void *a, const void *b) { long long x = *(const long long *)a, y = *(const long long *)b; return x < y ? -1 : x > y; }

int main(void)
{
	static const char *name[] = { "timerfd read", "timerfd epoll", "timerfd abstime", "posix timer" };
	int bad = 0, tfd = timerfd_create(CLOCK_MONOTONIC, 0), ep = epoll_create1(0);
	struct epoll_event e = { .events = EPOLLIN };
	epoll_ctl(ep, EPOLL_CTL_ADD, tfd, &e);
	sigset_t set;
	sigemptyset(&set); sigaddset(&set, SIGRTMIN);
	sigprocmask(SIG_BLOCK, &set, NULL);
	struct sigevent sev = { .sigev_notify = SIGEV_SIGNAL, .sigev_signo = SIGRTMIN };
	timer_t pt;
	timer_create(CLOCK_MONOTONIC, &sev, &pt);
	static long long late[30];
	for (int how = 0; how < 4; how++) {
		int early = 0;
		for (int i = 0; i < 30; i++) {
			struct itimerspec its = { .it_value = { 0, 1000000 } };
			uint64_t n;
			long long a = ns();
			if (how == 2) { its.it_value.tv_sec = (a + 1000000) / 1000000000; its.it_value.tv_nsec = (a + 1000000) % 1000000000; }
			if (how == 3) timer_settime(pt, 0, &its, NULL);
			else timerfd_settime(tfd, how == 2 ? TFD_TIMER_ABSTIME : 0, &its, NULL);
			if (how == 1) epoll_wait(ep, &e, 1, 1000);
			if (how == 3) sigwaitinfo(&set, NULL);
			else read(tfd, &n, sizeof(n));
			late[i] = ns() - a - 1000000;
			if (late[i] < 0) early++;
		}
		qsort(late, 30, sizeof(late[0]), cmp);
		if (early) { printf("%s: fired early %d times\n", name[how], early); bad++; }
		if (late[15] > 400000) { printf("%s: median lateness %lld ns\n", name[how], late[15]); bad++; }
	}
	puts(bad ? "FAIL" : "OK");
	return bad != 0;
}
