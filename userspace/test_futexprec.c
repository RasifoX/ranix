/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* A futex wait and sigtimedwait time out when asked: never early, median
 * overshoot under 400 us (LTP allows 400 us for a context switch) for 1 ms (LTP futex_wait05; pthread timed waits and
 * sigtimedwait share the path). */
#define _GNU_SOURCE
#include <linux/futex.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static long long ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000000000LL + t.tv_nsec; }
static int cmp(const void *a, const void *b) { long long x = *(const long long *)a, y = *(const long long *)b; return x < y ? -1 : x > y; }

int main(void)
{
	int bad = 0, word = 0;
	static long long over[40];
	sigset_t set;
	sigemptyset(&set); sigaddset(&set, SIGUSR1);
	sigprocmask(SIG_BLOCK, &set, NULL);
	for (int how = 0; how < 2; how++) {
		int early = 0;
		for (int i = 0; i < 40; i++) {
			struct timespec d = { 0, 1000000 };
			long long a = ns();
			if (how == 0) syscall(SYS_futex, &word, FUTEX_WAIT_PRIVATE, 0, &d, NULL, 0);
			else sigtimedwait(&set, NULL, &d);
			over[i] = ns() - a - 1000000;
			if (over[i] < 0) early++;
		}
		qsort(over, 40, sizeof(over[0]), cmp);
		const char *n = how ? "sigtimedwait" : "futex wait";
		if (early) { printf("%s 1 ms: ended early %d times\n", n, early); bad++; }
		if (over[20] > 400000) { printf("%s 1 ms: median overshoot %lld ns\n", n, over[20]); bad++; }
	}
	puts(bad ? "FAIL" : "OK");
	return bad != 0;
}
