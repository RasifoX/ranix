/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* SysV semaphore semantics: every sleeper is woken, values stay in
 * 0..SEMVMX (ERANGE otherwise), sem_op 0 waits for zero, SETALL wakes
 * sleepers, and one IPC namespace cannot take every array. */
#define _GNU_SOURCE
#include <errno.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <sys/ipc.h>
#include <sys/sem.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef SETVAL
#define SETVAL 16
#endif
static int fails;
#define FAIL(...) do { printf("FAIL " __VA_ARGS__); printf("\n"); fails++; } while (0)

static long sctl(int id, int n, int cmd, unsigned long arg)
{
	return syscall(SYS_semctl, id, n, cmd, arg);
}
static int op(int id, short num, short val, short flg)
{
	struct sembuf b = { (unsigned short)num, val, flg };
	return semop(id, &b, 1);
}
/* 1 if p exited within ms */
static int reaped(pid_t p, int ms)
{
	for (int i = 0; i < ms / 10; i++) {
		int st;
		pid_t w = waitpid(p, &st, WNOHANG);
		if (w == p) return 1;
		usleep(10000);
	}
	return 0;
}
static pid_t sleeper(int id, short val)
{
	pid_t p = fork();
	if (p == 0) { alarm(10); _exit(op(id, 0, val, 0) == 0 ? 0 : 1); }
	return p;
}
static void reap_or_kill(pid_t p, const char *what)
{
	if (!reaped(p, 3000)) { FAIL("%s: sleeper never woke", what); kill(p, SIGKILL); waitpid(p, NULL, 0); }
}

int main(void)
{
	int id = semget(IPC_PRIVATE, 1, IPC_CREAT | 0600);
	if (id < 0) { printf("FAIL semget errno=%d\n", errno); return 1; }

	/* two sleepers on one semaphore, one post of 2 */
	pid_t a = sleeper(id, -1), b = sleeper(id, -1);
	usleep(300000);
	if (op(id, 0, 2, 0) != 0) FAIL("post errno=%d", errno);
	reap_or_kill(a, "first of two");
	reap_or_kill(b, "second of two");

	/* SEMVMX = 32767 */
	if (sctl(id, 0, SETVAL, 32767) != 0) FAIL("setval max errno=%d", errno);
	errno = 0;
	if (op(id, 0, 1, IPC_NOWAIT) != -1 || errno != ERANGE) FAIL("semop past max: errno=%d", errno);
	if (sctl(id, 0, GETVAL, 0) != 32767) FAIL("value after overflow=%ld", sctl(id, 0, GETVAL, 0));
	errno = 0;
	if (sctl(id, 0, SETVAL, 32768) != -1 || errno != ERANGE) FAIL("setval 32768 errno=%d", errno);
	errno = 0;
	if (sctl(id, 0, SETVAL, (unsigned long)-1) != -1 || errno != ERANGE) FAIL("setval -1 errno=%d", errno);
	unsigned short big[1] = { 40000 };
	errno = 0;
	if (sctl(id, 0, SETALL, (unsigned long)big) != -1 || errno != ERANGE) FAIL("setall 40000 errno=%d", errno);

	/* sem_op 0: wait until the value is zero */
	sctl(id, 0, SETVAL, 1);
	errno = 0;
	if (op(id, 0, 0, IPC_NOWAIT) != -1 || errno != EAGAIN) FAIL("zero-wait nowait errno=%d", errno);
	pid_t z = sleeper(id, 0);
	usleep(300000);
	if (reaped(z, 10)) FAIL("zero-wait returned while value was 1");
	sctl(id, 0, SETVAL, 0);
	reap_or_kill(z, "zero-wait");

	/* two ops on one semaphore in one call see each other */
	sctl(id, 0, SETVAL, 1);
	struct sembuf two[2] = { { 0, -1, IPC_NOWAIT }, { 0, -1, IPC_NOWAIT } };
	errno = 0;
	if (semop(id, two, 2) != -1 || errno != EAGAIN) FAIL("double take of 1: errno=%d", errno);
	if (sctl(id, 0, GETVAL, 0) != 1) FAIL("double take left value=%ld", sctl(id, 0, GETVAL, 0));
	sctl(id, 0, SETVAL, 0);

	/* SETALL must wake a sleeper it satisfies */
	pid_t s = sleeper(id, -1);
	usleep(300000);
	unsigned short one[1] = { 1 };
	if (sctl(id, 0, SETALL, (unsigned long)one) != 0) FAIL("setall errno=%d", errno);
	reap_or_kill(s, "setall");
	sctl(id, 0, IPC_RMID, 0);

	/* a child IPC namespace gets a share, the first namespace keeps the rest */
	pid_t c = fork();
	if (c == 0) {
		if (unshare(CLONE_NEWIPC) != 0) _exit(100);
		int n = 0;
		while (n < 64 && semget(IPC_PRIVATE, 1, IPC_CREAT | 0600) >= 0) n++;
		int e = errno;
		int other = fork();
		if (other == 0) _exit(0);
		/* still held while we check from outside */
		printf("ns-arrays=%d errno=%d\n", n, e);
		fflush(stdout);
		pause();
		_exit(0);
	}
	usleep(500000);
	int mine = semget(IPC_PRIVATE, 1, IPC_CREAT | 0600);
	if (mine < 0) FAIL("first namespace starved by a child namespace errno=%d", errno);
	else sctl(mine, 0, IPC_RMID, 0);
	kill(c, SIGKILL);
	waitpid(c, NULL, 0);
	while (waitpid(-1, NULL, WNOHANG) > 0) {}

	if (!fails) puts("OK");
	return fails ? 1 : 0;
}
