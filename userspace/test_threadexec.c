/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* execve from a thread replaces the whole process: the other threads die and the
 * pid stays the process's. And a leader that exits with pthread_exit leaves the
 * process alive until its last thread ends, for the parent as well. */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define WANT(c) do { if (!(c)) { printf("FAIL line %d st=%x\n", __LINE__, st); return 1; } } while (0)

static void *spin(void *a) { for (;;) usleep(1000); return a; }
static void *do_exec(void *a)
{
	usleep(20000);
	execl("/bin/sh", "sh", "-c", "exit 7", (char *)NULL);
	_exit(9);
	return a;
}
static void *late(void *a) { usleep(300000); _exit(5); return a; }

int main(void)
{
	int st = 0;
	pid_t c = fork();
	if (c == 0) {
		pthread_t t;
		pthread_create(&t, NULL, spin, NULL);
		pthread_create(&t, NULL, do_exec, NULL);
		for (;;) pause();
	}
	alarm(20);
	WANT(waitpid(c, &st, 0) == c && WIFEXITED(st) && WEXITSTATUS(st) == 7);

	c = fork();
	if (c == 0) {
		pthread_t t;
		pthread_create(&t, NULL, late, NULL);
		pthread_exit(NULL);
	}
	usleep(100000);
	WANT(waitpid(c, &st, WNOHANG) == 0);         /* its worker is still running */
	WANT(waitpid(c, &st, 0) == c && WIFEXITED(st) && WEXITSTATUS(st) == 5);
	puts("OK");
	return 0;
}
