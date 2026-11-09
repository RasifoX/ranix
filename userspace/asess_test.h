/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* Shared by the test_asess_* programs. */
#ifndef ASESS_TEST_H
#define ASESS_TEST_H

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include "../include/uapi/asess.h"

#define UID 1000
#define GID 1000

static int fails;

#define CHECK(cond, what) do { \
	if (!(cond)) { printf("FAIL %s (errno %d)\n", what, errno); fails++; } \
} while (0)

/* expr must fail with exactly this errno */
#define DENIED(expr, err, what) do { \
	errno = 0; \
	long r_ = (long)(expr); \
	if (r_ != -1 || errno != (err)) { \
		printf("FAIL %s: got %ld errno %d, want errno %d\n", what, r_, errno, err); \
		fails++; \
	} \
} while (0)

static inline long asess(unsigned op, int sfd, void *arg, size_t size)
{
	return syscall(ASESS_SYSCALL, op, sfd, arg, size);
}

static inline unsigned long long asess_self(void)
{
	return (unsigned long long)syscall(SYS_prctl, PR_RANIX_ASESS_ID, 0, 0, 0, 0);
}

static inline long sess_create(const char *root, unsigned uid, const int *keep, int nkeep,
                               unsigned max_tasks)
{
	struct asess_attr a;
	memset(&a, 0, sizeof(a));
	a.size = sizeof(a);
	a.uid = uid;
	a.gid = uid ? GID : 0;
	a.root = (unsigned long long)(unsigned long)root;
	a.keep_fds = (unsigned long long)(unsigned long)keep;
	a.nkeep = (unsigned)nkeep;
	a.max_tasks = max_tasks;
	strcpy(a.label, "test");
	return asess(ASESS_CREATE, -1, &a, sizeof(a));
}

static inline long sess_spawn(long sfd)
{
	return asess(ASESS_SPAWN, (int)sfd, NULL, 0);
}

/* as sess_spawn, for tests that cannot go on without a session */
static inline long sess_must_spawn(long sfd)
{
	long pid = sess_spawn(sfd);
	if (pid < 0) {
		printf("FAIL spawn: errno %d (a kept stdout must be a pipe, not a file)\n", errno);
		printf("FAILED\n");
		exit(1);
	}
	return pid;
}

/* folds a session's failures into the parent's */
static inline void sess_reap(pid_t pid)
{
	int st = 0;
	if (pid <= 0 || waitpid(pid, &st, 0) != pid) { printf("FAIL waitpid\n"); fails++; return; }
	if (!WIFEXITED(st)) { printf("FAIL session died by signal %d\n", WTERMSIG(st)); fails++; }
	else if (WEXITSTATUS(st)) { printf("FAIL session reported %d\n", WEXITSTATUS(st)); fails++; }
}

static inline int finish(void)
{
	if (fails) { printf("FAILED %d\n", fails); return 1; }
	puts("OK");
	return 0;
}

#endif
