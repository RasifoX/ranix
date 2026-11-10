/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* poll, select, ppoll, pselect, epoll_wait and epoll_pwait2 time out when asked:
 * never early, and with a median overshoot under 400 us (LTP allows 400 us for a context switch) for 1 ms (LTP poll02,
 * select02, pselect01, epoll_wait02, epoll_pwait03). A 0.3 ms ppoll or
 * epoll_pwait2 waits (it is not a 0 ms poll). */
#define _GNU_SOURCE
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <sys/select.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static long long ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000000000LL + t.tv_nsec; }
static int cmp(const void *a, const void *b) { long long x = *(const long long *)a, y = *(const long long *)b; return x < y ? -1 : x > y; }

static int ep, p[2];

static void wait_once(int how, long want)
{
	struct timespec ts = { 0, want };
	struct timeval tv = { 0, want / 1000 };
	struct pollfd pf = { .fd = p[0], .events = POLLIN };
	struct epoll_event e;
	fd_set r;
	FD_ZERO(&r); FD_SET(p[0], &r);
	switch (how) {
	case 0: poll(&pf, 1, (int)(want / 1000000)); break;
	case 1: select(p[0] + 1, &r, NULL, NULL, &tv); break;
	case 2: ppoll(&pf, 1, &ts, NULL); break;
	case 3: pselect(p[0] + 1, &r, NULL, NULL, &ts, NULL); break;
	case 4: epoll_wait(ep, &e, 1, (int)(want / 1000000)); break;
	case 5: syscall(441 /* epoll_pwait2 (x86_64, aarch64) */, ep, &e, 1, &ts, NULL, 8); break;
	}
}

int main(void)
{
	static const char *name[] = { "poll", "select", "ppoll", "pselect", "epoll_wait", "epoll_pwait2" };
	int bad = 0;
	pipe(p);
	ep = epoll_create1(0);
	struct epoll_event e = { .events = EPOLLIN };
	epoll_ctl(ep, EPOLL_CTL_ADD, p[0], &e);
	static long long over[40];
	for (int how = 0; how < 6; how++) {
		long wants[2] = { 1000000, 300000 };
		for (int w = 0; w < (how == 2 || how == 3 || how == 5 ? 2 : 1); w++) {
			int early = 0;
			for (int i = 0; i < 40; i++) {
				long long a = ns();
				wait_once(how, wants[w]);
				over[i] = ns() - a - wants[w];
				if (over[i] < 0) early++;
			}
			qsort(over, 40, sizeof(over[0]), cmp);
			if (early) { printf("%s %ld ns: ended early %d times\n", name[how], wants[w], early); bad++; }
			if (over[20] > 400000) { printf("%s %ld ns: median overshoot %lld ns\n", name[how], wants[w], over[20]); bad++; }
		}
	}
	puts(bad ? "FAIL" : "OK");
	return bad != 0;
}
