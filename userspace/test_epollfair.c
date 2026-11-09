/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* More fds stay ready than maxevents: epoll_wait takes turns, so over four
 * rounds each of 13 ready pipes is reported exactly 4 times (LTP epoll_wait14). */
#include <stdio.h>
#include <sys/epoll.h>
#include <unistd.h>

#define NREADY 13
#define MAXEV  3
#define ROUNDS 4

int main(void)
{
	int ep = epoll_create1(0), count[NREADY] = { 0 }, bad = 0;
	for (int i = 0; i < NREADY; i++) {
		int p[2];
		if (pipe(p) || write(p[1], "x", 1) != 1) { puts("setup failed"); return 1; }
		struct epoll_event ev = { .events = EPOLLIN, .data.u64 = (unsigned)i };
		epoll_ctl(ep, EPOLL_CTL_ADD, p[0], &ev);
	}
	struct epoll_event got[MAXEV];
	for (int n = 0; n < NREADY * ROUNDS; ) {
		int r = epoll_wait(ep, got, MAXEV, 0);
		if (r != MAXEV) { printf("epoll_wait returned %d, want %d\n", r, MAXEV); return 1; }
		for (int i = 0; i < r && n < NREADY * ROUNDS; i++, n++) count[got[i].data.u64]++;
	}
	for (int i = 0; i < NREADY; i++)
		if (count[i] != ROUNDS) { printf("fd %d reported %d times, want %d\n", i, count[i], ROUNDS); bad++; }
	puts(bad ? "FAIL" : "OK");
	return bad != 0;
}
