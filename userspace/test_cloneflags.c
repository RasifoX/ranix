/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* clone flags on a fork-like clone (LTP clone08): CLONE_PARENT makes the
 * child a sibling of the caller (its parent is the caller's parent, who
 * reaps it); CLONE_CHILD_SETTID writes the child's tid into the child's own
 * copy of memory before it runs, and leaves the parent's copy alone. */
#define _GNU_SOURCE
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

static long raw_clone(unsigned long flags, int *ctid)
{
#if defined(__aarch64__)
	return syscall(SYS_clone, flags, 0, NULL, 0, ctid);   /* flags, stack, ptid, tls, ctid */
#else
	return syscall(SYS_clone, flags, 0, NULL, ctid, 0);   /* flags, stack, ptid, ctid, tls */
#endif
}

static volatile int ctid;

int main(void)
{
	int bad = 0, st;
	pid_t me = getpid();

	/* CLONE_PARENT: the grandchild reports its parent through its exit code */
	pid_t x = fork();
	if (x == 0) {
		long g = raw_clone(CLONE_PARENT | SIGCHLD, NULL);
		if (g == 0) _exit(getppid() == me ? 0 : 1);
		_exit(g > 0 ? 0 : 2);
	}
	int seen = 0;
	for (int i = 0; i < 2; i++) {
		pid_t w = waitpid(-1, &st, 0);
		if (w <= 0) { printf("CLONE_PARENT: wait %d found nothing (the sibling is not mine)\n", i + 1); bad++; break; }
		seen++;
		if (!WIFEXITED(st) || WEXITSTATUS(st)) { printf("CLONE_PARENT: pid %d status 0x%x\n", w, st); bad++; }
	}

	/* CLONE_CHILD_SETTID without CLONE_VM */
	ctid = 0;
	long c = raw_clone(CLONE_CHILD_SETTID | SIGCHLD, (int *)&ctid);
	if (c == 0) _exit(ctid == getpid() ? 0 : 1);
	waitpid(c, &st, 0);
	if (!WIFEXITED(st) || WEXITSTATUS(st)) { puts("CLONE_CHILD_SETTID: the child's ctid is not its tid"); bad++; }
	if (ctid != 0) { printf("CLONE_CHILD_SETTID: the parent's copy changed to %d\n", ctid); bad++; }
	puts(bad ? "FAIL" : "OK");
	return bad != 0;
}
