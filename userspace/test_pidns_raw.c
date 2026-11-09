/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* Inside a pid namespace every syscall taking a pid resolves it there: a global
 * number of an outside task is ESRCH, local numbers work, and the numbers the
 * kernel hands back are local too. Raw syscalls: musl stubs some sched_* out. */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/prctl.h>
#include <sys/ptrace.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/wait.h>

static int esrch(long r) { return r == -1 && errno == ESRCH; }

static pid_t sleeper(void)
{
	pid_t c = fork();
	if (c == 0) {
		prctl(PR_SET_PDEATHSIG, SIGKILL);   /* must not outlive a failed run */
		for (;;) pause();
	}
	return c;
}

static int outside_hidden(pid_t t)
{
	long m[16], lim[2], self = 0;
	struct sched_param sp = { 0 };
	void *head; size_t len;
	struct iovec l = { &self, sizeof(self) }, r = { &self, sizeof(self) };
	memset(m, 0xff, sizeof(m));
	if (!esrch(syscall(SYS_getpgid, t))) return 11;
	if (!esrch(syscall(SYS_getsid, t))) return 12;
	if (!esrch(syscall(SYS_setpgid, t, 0))) return 13;
	if (!esrch(syscall(SYS_sched_getscheduler, t))) return 14;
	if (!esrch(syscall(SYS_sched_setscheduler, t, SCHED_OTHER, &sp))) return 15;
	if (!esrch(syscall(SYS_sched_getparam, t, &sp))) return 16;
	if (!esrch(syscall(SYS_sched_setparam, t, &sp))) return 17;
	if (!esrch(syscall(SYS_sched_getaffinity, t, sizeof(m), m))) return 18;
	if (!esrch(syscall(SYS_sched_setaffinity, t, sizeof(m), m))) return 19;
	if (!esrch(syscall(SYS_prlimit64, t, RLIMIT_NOFILE, 0, lim))) return 20;
	if (!esrch(syscall(SYS_get_robust_list, t, &head, &len))) return 21;
	if (!esrch(syscall(SYS_pidfd_open, t, 0))) return 22;
	if (!esrch(syscall(SYS_process_vm_readv, t, &l, 1, &r, 1, 0))) return 23;
	if (!esrch(syscall(SYS_ptrace, PTRACE_ATTACH, t, 0, 0))) return 24;
	return 0;
}

static int local_found(pid_t t)
{
	long m[16], lim[2];
	if (syscall(SYS_getpgid, t) < 0) return 30;
	if (syscall(SYS_sched_getscheduler, t) < 0) return 31;
	if (syscall(SYS_sched_getaffinity, t, sizeof(m), m) < 0) return 32;
	if (syscall(SYS_prlimit64, t, RLIMIT_NOFILE, 0, lim) < 0) return 33;
	if (syscall(SYS_pidfd_open, t, 0) < 0) return 34;
	return 0;
}

static void *thread_fn(void *a)
{
	long m[16];
	long tid = syscall(SYS_gettid);
	(void)a;
	if (tid <= 1 || tid > 16) return (void *)1;      /* a local number */
	if (syscall(SYS_tgkill, getpid(), tid, 0)) return (void *)2;
	if (syscall(SYS_sched_getaffinity, tid, sizeof(m), m) < 0) return (void *)3;
	return 0;
}

static int inside(pid_t outside)
{
	if (getpid() != 1 || syscall(SYS_gettid) != 1) return 2;
	pid_t in = sleeper();
	if (in != 2) return 3;                  /* fork returns the local number */
	int r = outside_hidden(outside);
	if (!r) r = local_found(in);
	if (r) return r;

	/* process groups are numbered locally as well */
	if (setpgid(in, in)) return 6;
	if (getpgid(in) != in || kill(-in, 0)) return 7;
	if (setpgid(0, 0) || getpgrp() != 1 || getpgid(0) != 1) return 8;

	pthread_t t; void *tr = (void *)9;
	if (pthread_create(&t, 0, thread_fn, 0) || pthread_join(t, &tr)) return 9;
	if (tr) return 40 + (int)(long)tr;

	int st;
	kill(in, SIGKILL);
	if (waitpid(in, &st, 0) != in) return 10;
	return 0;
}

int main(void)
{
	pid_t outside = sleeper();
	long c = syscall(SYS_clone, CLONE_NEWPID | SIGCHLD, 0, 0, 0, 0);
	if (c == 0) _exit(inside(outside));
	int st = -1;
	waitpid((pid_t)c, &st, 0);
	kill(outside, SIGKILL);
	waitpid(outside, 0, 0);
	if (WIFEXITED(st) && WEXITSTATUS(st) == 0) { puts("OK"); return 0; }
	printf("FAIL %d\n", WIFEXITED(st) ? WEXITSTATUS(st) : -1);
	return 1;
}
