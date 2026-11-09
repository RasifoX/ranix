/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* waitpid by process group (LTP waitpid10-13, clone07): pid 0 waits for any
 * child in the caller's group, pid < -1 for any child in group -pid, and
 * waitid(P_PGID, 0) for the caller's group. A child in another group is not
 * the caller's to reap with pid 0. */
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

static int bad;

/* a child that exits with 3 once *go is closed (NULL: right away) */
static pid_t kid(int own_group, int *go)
{
	int p[2];
	if (pipe(p)) return -1;
	pid_t c = fork();
	if (c == 0) {
		if (own_group) setpgid(0, 0);
		char x;
		close(p[1]);
		read(p[0], &x, 1);   /* exit once the parent has set things up */
		_exit(3);
	}
	if (own_group) setpgid(c, c);
	close(p[0]);
	if (go) *go = p[1]; else close(p[1]);
	return c;
}

static void want(const char *what, long got, long exp, const int *stp)
{
	int st = *stp;   /* read after the wait, not as an argument beside it */
	if (got != exp) { printf("%s: got %ld (errno %d), want %ld\n", what, got, errno, exp); bad++; }
	else if (exp > 0 && (!WIFEXITED(st) || WEXITSTATUS(st) != 3)) { printf("%s: status 0x%x\n", what, st); bad++; }
}

int main(void)
{
	int st = 0;
	pid_t a = kid(0, NULL);
	errno = 0; want("waitpid(0) reaps a child in my group", waitpid(0, &st, 0), a, &st);

	pid_t b = kid(1, NULL);
	errno = 0; want("waitpid(0) with only a child in another group", waitpid(0, &st, 0), -1, &st);
	if (errno != ECHILD) { printf("  errno %d, want ECHILD\n", errno); bad++; }
	errno = 0; want("waitpid(-pgid) reaps it", waitpid(-b, &st, 0), b, &st);

	errno = 0; want("waitpid(-group with no children)", waitpid(-0x7ffff, &st, WNOHANG), -1, &st);

	int go;
	pid_t c = kid(0, &go);
	errno = 0; want("waitpid(0, WNOHANG) of a live child", waitpid(0, &st, WNOHANG), 0, &st);
	close(go);
	siginfo_t si = { 0 };
	errno = 0;
	if (waitid(P_PGID, 0, &si, WEXITED) != 0 || si.si_pid != c) {
		printf("waitid(P_PGID, 0): pid %d (errno %d), want %d\n", si.si_pid, errno, c); bad++;
	}
	puts(bad ? "FAIL" : "OK");
	return bad != 0;
}
