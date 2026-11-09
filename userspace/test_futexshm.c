/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* A futex in a MAP_SHARED page is one word for every process that maps it: a
 * FUTEX_WAKE from a forked child reaches the parent waiting there, for shared
 * anonymous memory and for a file mapping alike. LTP's checkpoints (and
 * process-shared pthread objects) work this way; the waker does not change the
 * word, it wakes until one waiter is woken. A private page stays private: the
 * child's wake must not reach the parent's copy. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/futex.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static long fut(volatile int *a, int op, int val, struct timespec *t)
{
	return syscall(SYS_futex, a, op, val, t, NULL, 0);
}

/* the child wakes until somebody was woken (at most 2 s); the parent waits 3 s */
static int pair(volatile int *w, const char *what, int want_woken)
{
	pid_t k = fork();
	if (k == 0) {
		for (int i = 0; i < 200; i++) {
			if (fut(w, FUTEX_WAKE, 1, NULL) == 1) _exit(1);
			usleep(10000);
		}
		_exit(0);
	}
	struct timespec t = { 3, 0 };
	long r = fut(w, FUTEX_WAIT, 0, &t);
	int e = errno, st = 0;
	waitpid(k, &st, 0);
	int woken = r == 0, child_woke = WIFEXITED(st) && WEXITSTATUS(st) == 1;
	if (woken != want_woken || child_woke != want_woken) {
		printf("%s: wait %ld (%d), child woke %d, want %d\n", what, r, r ? e : 0, child_woke, want_woken);
		return 1;
	}
	return 0;
}

int main(void)
{
	int bad = 0;
	volatile int *anon = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	int fd = open("/tmp/futexshm", O_CREAT | O_RDWR | O_TRUNC, 0600);
	ftruncate(fd, 4096);
	volatile int *file = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	volatile int *priv = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (anon == MAP_FAILED || file == MAP_FAILED || priv == MAP_FAILED) { puts("mmap FAIL"); return 1; }
	*anon = 0; *file = 0; *priv = 0;
	bad += pair(anon, "shared anon", 1);
	bad += pair(file, "shared file", 1);
	bad += pair(priv, "private", 0);
	unlink("/tmp/futexshm");
	puts(bad ? "FAIL" : "OK");
	return bad != 0;
}
