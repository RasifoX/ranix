/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* Creating namespaces takes CAP_SYS_ADMIN in the initial user namespace. Without
 * it, the way in is CLONE_NEWUSER, alone or in the same call. */
#define _GNU_SOURCE
#include <errno.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>

#ifndef CLONE_NEWTIME
#define CLONE_NEWTIME 0x80
#endif

static const unsigned long flags[] = {
	CLONE_NEWNS, CLONE_NEWUTS, CLONE_NEWIPC, CLONE_NEWPID,
	CLONE_NEWNET, CLONE_NEWCGROUP, CLONE_NEWTIME,
};
#define NFLAGS (int)(sizeof(flags) / sizeof(flags[0]))

static int unpriv(void)
{
	if (setuid(64003)) return 1;
	for (int i = 0; i < NFLAGS; i++)
		if (unshare(flags[i]) != -1 || errno != EPERM) return 10 + i;
	for (int i = 0; i < NFLAGS - 1; i++) {   /* legacy clone has no NEWTIME */
		long c = syscall(SYS_clone, flags[i] | SIGCHLD, 0, 0, 0, 0);
		if (c == 0) _exit(0);
		if (c != -1 || errno != EPERM) return 20 + i;
	}
	return 0;
}

static int rootless(unsigned long first)
{
	if (setuid(64003)) return 1;
	if (unshare(first)) return 2;
	for (int i = 0; i < NFLAGS; i++)
		if (unshare(flags[i])) return 10 + i;
	return 0;
}

static int run(int (*fn)(unsigned long), unsigned long a)
{
	pid_t c = fork();
	if (c == 0) _exit(fn(a));
	int st = 0;
	waitpid(c, &st, 0);
	return WIFEXITED(st) ? WEXITSTATUS(st) : 99;
}

static int call_unpriv(unsigned long a) { (void)a; return unpriv(); }

/* the same call carries every flag, and clone may too */
static int bundled(unsigned long a)
{
	(void)a;
	if (setuid(64003)) return 1;
	if (unshare(CLONE_NEWUSER | CLONE_NEWNS | CLONE_NEWUTS | CLONE_NEWNET)) return 2;
	long c = syscall(SYS_clone, CLONE_NEWUSER | CLONE_NEWPID | SIGCHLD, 0, 0, 0, 0);
	if (c == 0) _exit(getpid() == 1 ? 0 : 3);
	if (c < 0) return 4;
	int st = 0;
	waitpid(c, &st, 0);
	return WEXITSTATUS(st);
}

/* root in the initial namespace, no capabilities left */
static int dropped(unsigned long a)
{
	(void)a;
	struct { unsigned v; int pid; } h = { 0x20080522, 0 };
	struct { unsigned e, p, i; } d[2] = {{0}};
	if (syscall(SYS_capset, &h, d)) return 1;
	return unshare(CLONE_NEWNS) == -1 && errno == EPERM ? 0 : 2;
}

int main(void)
{
	int r;
	if ((r = run(call_unpriv, 0))) { printf("FAIL unpriv rc=%d\n", r); return 1; }
	printf("unpriv ok\n");
	if ((r = run(rootless, CLONE_NEWUSER))) { printf("FAIL rootless rc=%d\n", r); return 1; }
	printf("rootless ok\n");
	if ((r = run(bundled, 0))) { printf("FAIL bundled rc=%d\n", r); return 1; }
	printf("bundled ok\n");
	if ((r = run(dropped, 0))) { printf("FAIL dropped rc=%d\n", r); return 1; }
	printf("dropped ok\n");
	/* root is untouched */
	for (int i = 0; i < NFLAGS; i++) {
		pid_t c = fork();
		if (c == 0) _exit(unshare(flags[i]) ? 1 : 0);
		int st;
		waitpid(c, &st, 0);
		if (WEXITSTATUS(st)) { printf("FAIL root %d\n", i); return 1; }
	}
	printf("OK\n");
	return 0;
}
