/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* Credentials and limits (audit step 2, items #3 and #4).
 *  - Limits are the thread group's: a thread sees the limit its group set, and a
 *    limit set from a thread holds for the whole group.
 *  - task_may_control: a user may not re-limit or renice a setuid-root process
 *    it started (uid=user, euid=suid=0); its own plain child it may.
 * Prints OK, or FAIL with the first thing that went wrong. Runs as root. */
#define _GNU_SOURCE
#include <errno.h>
#include <grp.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/resource.h>
#include <sys/wait.h>

static volatile int g_seen = -1;

static void *thread_main(void *arg)
{
	(void)arg;
	struct rlimit r;
	if (getrlimit(RLIMIT_NPROC, &r) == 0) g_seen = (int)r.rlim_cur;
	struct rlimit z = { 0, 0 };
	setrlimit(RLIMIT_CORE, &z);              /* set here, must hold for main too */
	return NULL;
}

static int limits_group(void)
{
	struct rlimit n = { 5, 5 };
	if (setrlimit(RLIMIT_NPROC, &n) != 0) return 1;
	pthread_t t;
	if (pthread_create(&t, NULL, thread_main, NULL) != 0) return 2;
	pthread_join(t, NULL);
	if (g_seen != 5) return 3;               /* the thread started with no limit */
	struct rlimit c;
	if (getrlimit(RLIMIT_CORE, &c) != 0 || c.rlim_cur != 0) return 4;   /* thread's change lost */
	return 0;
}

static int become(uid_t u)
{
	return setgroups(0, NULL) || setgid(u) || setuid(u);
}

int main(void)
{
	if (geteuid() != 0) { puts("FAIL run as root"); return 0; }
	int st;
	pid_t p = fork();
	if (p == 0) _exit(limits_group());
	waitpid(p, &st, 0);
	if (!WIFEXITED(st) || WEXITSTATUS(st)) {
		static const char *const why[] = { "", "setrlimit", "pthread", "thread did not inherit the group limit",
		                                   "a thread's setrlimit did not reach the group" };
		int c = WIFEXITED(st) ? WEXITSTATUS(st) : 0;
		printf("FAIL limits: %s\n", c > 0 && c < 5 ? why[c] : "crashed");
		return 0;
	}

	/* a setuid-root process started by uid 1000: uid 1000, euid and suid 0 */
	int ready[2];
	if (pipe(ready)) { puts("FAIL pipe"); return 0; }
	pid_t victim = fork();
	if (victim == 0) {
		if (setresuid(1000, 0, 0) != 0) _exit(1);
		if (write(ready[1], "x", 1) != 1) _exit(1);
		pause();
		_exit(0);
	}
	char c;
	if (read(ready[0], &c, 1) != 1) { puts("FAIL victim"); kill(victim, SIGKILL); return 0; }

	pid_t a = fork();
	if (a == 0) {
		if (become(1000)) _exit(10);
		struct rlimit nl = { 4, 4 };
		if (prlimit(victim, RLIMIT_NOFILE, &nl, NULL) == 0) _exit(11);
		if (errno != EPERM) _exit(12);
		if (setpriority(PRIO_PROCESS, victim, 10) == 0) _exit(13);
		/* its own plain child stays its to control */
		pid_t own = fork();
		if (own == 0) { pause(); _exit(0); }
		int rc = prlimit(own, RLIMIT_NOFILE, &nl, NULL) == 0 ? 0 : 14;
		if (!rc && (setpriority(PRIO_PROCESS, own, 7) != 0 || getpriority(PRIO_PROCESS, own) != 7)) rc = 15;
		/* naming itself by pid is the same as who=0 */
		if (!rc && (setpriority(PRIO_PROCESS, getpid(), 3) != 0 || getpriority(PRIO_PROCESS, 0) != 3)) rc = 16;
		kill(own, SIGKILL);
		waitpid(own, NULL, 0);
		_exit(rc);
	}
	waitpid(a, &st, 0);
	kill(victim, SIGKILL);
	waitpid(victim, NULL, 0);
	int code = WIFEXITED(st) ? WEXITSTATUS(st) : 99;
	switch (code) {
	case 0: puts("OK"); break;
	case 11: puts("FAIL user re-limited a setuid-root process it started"); break;
	case 12: printf("FAIL prlimit on setuid process: errno not EPERM\n"); break;
	case 13: puts("FAIL user reniced a setuid-root process it started"); break;
	case 14: puts("FAIL user could not re-limit its own child"); break;
	case 15: puts("FAIL user could not renice its own child by pid"); break;
	case 16: puts("FAIL setpriority by own pid did not act on the caller"); break;
	default: printf("FAIL credctl code %d\n", code); break;
	}
	return 0;
}
