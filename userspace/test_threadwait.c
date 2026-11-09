/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* Children belong to the thread group, not to the thread that forked them: any
 * thread may wait for them, and they outlive the thread. A thread's own exit is no
 * SIGCHLD, and its parent is the group's parent. */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

static volatile sig_atomic_t chld;
static void on_chld(int s) { (void)s; chld++; }
static pid_t forked, thppid;
static int waited_st;
static pid_t waited;
static int waited_err;

static void *fork_exit(void *a)            /* fork and leave at once */
{
	pid_t c = fork();
	if (c == 0) { usleep(100000); _exit(getppid() == (pid_t)(long)a ? 5 : 6); }
	forked = c;
	return NULL;
}

static void *fork_stay(void *a)
{
	pid_t c = fork();
	if (c == 0) _exit(getppid() == (pid_t)(long)a ? 5 : 6);
	forked = c;
	return NULL;
}

static void *wait_any(void *a)             /* block in wait for main's child */
{
	(void)a;
	waited = waitpid(-1, &waited_st, 0);
	waited_err = errno;
	return NULL;
}

static void *ppid_of(void *a) { (void)a; thppid = getppid(); return NULL; }
static void *nop(void *a) { return a; }

static int ok5(pid_t got, pid_t want, int st)
{
	return got == want && WIFEXITED(st) && WEXITSTATUS(st) == 5;
}

int main(void)
{
	pthread_t t;
	int st;
	void *self = (void *)(long)getpid();

	pthread_create(&t, NULL, fork_stay, self); pthread_join(t, NULL);
	pid_t r = waitpid(forked, &st, 0);
	if (!ok5(r, forked, st)) { printf("FAIL by pid %d st %x\n", r, st); return 1; }

	pthread_create(&t, NULL, fork_stay, self); pthread_join(t, NULL);
	r = waitpid(-1, &st, 0);
	if (!ok5(r, forked, st)) { printf("FAIL any %d st %x\n", r, st); return 1; }

	pthread_create(&t, NULL, fork_exit, self); pthread_join(t, NULL);
	r = waitpid(-1, &st, 0);                /* the forking thread is gone */
	if (!ok5(r, forked, st)) { printf("FAIL orphaned %d st %x\n", r, st); return 1; }

	pid_t c = fork();
	if (c == 0) { usleep(200000); _exit(5); }   /* the thread sleeps in wait first */
	pthread_create(&t, NULL, wait_any, NULL);
	pthread_join(t, NULL);
	if (!ok5(waited, c, waited_st)) { printf("FAIL sibling wait %d errno %d\n", waited, waited_err); return 1; }

	pthread_create(&t, NULL, ppid_of, NULL); pthread_join(t, NULL);
	if (thppid != getppid()) { printf("FAIL thread ppid %d\n", thppid); return 1; }

	signal(SIGCHLD, on_chld);
	pthread_create(&t, NULL, nop, NULL); pthread_join(t, NULL);
	usleep(50000);
	if (chld) { puts("FAIL SIGCHLD from thread"); return 1; }
	puts("OK");
	return 0;
}
