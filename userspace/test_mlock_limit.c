/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* Locked memory cannot be paged out or reclaimed, so an unprivileged process
 * may lock only up to RLIMIT_MEMLOCK (ENOMEM above it, EPERM when the limit
 * is 0). mlockall(MCL_CURRENT) counts everything mapped. mmap(MAP_LOCKED) and
 * mappings after mlockall(MCL_FUTURE) past the limit are EAGAIN (EPERM for
 * MAP_LOCKED at limit 0). CAP_IPC_LOCK lifts the limit. A range with an
 * unmapped page is ENOMEM; bad flags are EINVAL.
 * Prints FAIL lines, then OK or FAIL <n>. Runs as root (drops to a user). */
#define _GNU_SOURCE
#include <errno.h>
#include <grp.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/wait.h>

#define USER 4321
#define BIG  (1024 * 1024)

static int fails;
#define CHECK(c, what) do { if (!(c)) { printf("FAIL %s (errno=%d)\n", what, errno); fails++; } } while (0)

static int user_with_limit(rlim_t lim)
{
	struct rlimit r = { lim, lim };
	if (setrlimit(RLIMIT_MEMLOCK, &r)) return -1;
	return setgroups(0, NULL) || setgid(USER) || setuid(USER) ? -1 : 0;
}

static int case_over_limit(void)
{
	if (user_with_limit(64 * 1024)) { puts("FAIL setup limit 64K"); return 1; }
	char *p = mmap(NULL, BIG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (p == MAP_FAILED) { puts("FAIL mmap"); return 1; }
	CHECK(mlock(p, 4096) == 0, "mlock of one page under the limit");
	errno = 0;
	CHECK(mlock(p, BIG) == -1 && errno == ENOMEM, "mlock past RLIMIT_MEMLOCK allowed");
	errno = 0;
	CHECK(syscall(SYS_mlock2, p, BIG, 0) == -1 && errno == ENOMEM, "mlock2 past RLIMIT_MEMLOCK allowed");
	errno = 0;
	CHECK(mlockall(MCL_CURRENT) == -1 && errno == ENOMEM, "mlockall(MCL_CURRENT) past RLIMIT_MEMLOCK allowed");
	munlockall();
	errno = 0;
	void *q = mmap(NULL, BIG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_LOCKED, -1, 0);
	CHECK(q == MAP_FAILED && errno == EAGAIN, "mmap(MAP_LOCKED) past RLIMIT_MEMLOCK allowed");
	if (q != MAP_FAILED) munmap(q, BIG);
	CHECK(mlockall(MCL_FUTURE) == 0, "mlockall(MCL_FUTURE) under a nonzero limit");
	errno = 0;
	q = mmap(NULL, BIG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	CHECK(q == MAP_FAILED && errno == EAGAIN, "mmap after MCL_FUTURE past RLIMIT_MEMLOCK allowed");
	if (q != MAP_FAILED) munmap(q, BIG);
	munlockall();
	return fails;
}

/* root keeps CAP_IPC_LOCK: the limit does not apply */
static int case_ipc_lock(void)
{
	struct rlimit r = { 64 * 1024, 64 * 1024 };
	if (setrlimit(RLIMIT_MEMLOCK, &r)) { puts("FAIL setup limit"); return 1; }
	char *p = mmap(NULL, BIG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (p == MAP_FAILED) { puts("FAIL mmap"); return 1; }
	CHECK(mlock(p, BIG) == 0, "CAP_IPC_LOCK: mlock past the limit refused");
	munlock(p, BIG);
	return fails;
}

static int case_zero_limit(void)
{
	if (user_with_limit(0)) { puts("FAIL setup limit 0"); return 1; }
	char *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (p == MAP_FAILED) { puts("FAIL mmap"); return 1; }
	errno = 0;
	CHECK(mlock(p, 4096) == -1 && errno == EPERM, "mlock with RLIMIT_MEMLOCK 0 allowed");
	errno = 0;
	/* the raw call: musl's mmap() turns an anonymous mapping's EPERM into ENOMEM */
	long q = syscall(SYS_mmap, NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_LOCKED, -1, 0);
	CHECK(q == -1 && errno == EPERM, "mmap(MAP_LOCKED) with RLIMIT_MEMLOCK 0 allowed");
	errno = 0;
	CHECK(mlockall(MCL_FUTURE) == -1 && errno == EPERM, "mlockall with RLIMIT_MEMLOCK 0 allowed");
	return fails;
}

static int case_args(void)
{
	long pg = sysconf(_SC_PAGESIZE);
	char *p = mmap(NULL, (size_t)pg * 3, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (p == MAP_FAILED) { puts("FAIL mmap"); return 1; }
	munmap(p + pg, (size_t)pg);
	errno = 0;
	CHECK(mlock(p, (size_t)pg * 3) == -1 && errno == ENOMEM, "mlock across an unmapped page");
	errno = 0;
	CHECK(syscall(SYS_mlock2, p, (size_t)pg, 0x80) == -1 && errno == EINVAL, "mlock2 with an unknown flag");
	errno = 0;
	CHECK(mlockall(0) == -1 && errno == EINVAL, "mlockall(0)");
	CHECK(munlock(p, (size_t)pg) == 0, "munlock of a mapped page");
	return fails;
}

static void run(const char *name, int (*fn)(void))
{
	fflush(stdout);
	pid_t p = fork();
	if (p == 0) { int r = fn(); fflush(stdout); _exit(r); }
	int st = 0;
	waitpid(p, &st, 0);
	if (!WIFEXITED(st)) { printf("FAIL %s crashed\n", name); fails++; }
	else fails += WEXITSTATUS(st);
}

int main(void)
{
	if (geteuid() != 0) { puts("FAIL run as root"); return 0; }
	run("over-limit", case_over_limit);
	run("zero-limit", case_zero_limit);
	run("ipc-lock", case_ipc_lock);
	run("args", case_args);
	if (fails) printf("FAIL %d\n", fails); else puts("OK");
	return 0;
}
