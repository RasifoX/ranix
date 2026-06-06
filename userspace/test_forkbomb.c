/* RLIMIT_NPROC: a non-root process may not fork past its soft cap on live
 * processes; fork returns EAGAIN at the limit and the count recovers once
 * children exit. Root is exempt. The suite runs as uid 0, so the bounded half
 * runs in a child that drops to uid 1000 first. */
#define _GNU_SOURCE
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <sys/resource.h>

#define CAP 8

/* fork children that park in pause() until killed; return how many forks
 * succeeded before EAGAIN, recording the pids. */
static int fork_until_eagain(pid_t *pids, int max, int *saw_eagain)
{
	int n = 0;
	*saw_eagain = 0;
	while (n < max) {
		pid_t c = fork();
		if (c == 0) { for (;;) pause(); _exit(0); }
		if (c < 0) { if (errno == EAGAIN) *saw_eagain = 1; break; }
		pids[n++] = c;
	}
	return n;
}

/* the non-root body: runs after setuid(1000) in a child of the suite */
static int bounded_body(void)
{
	/* lower only the soft cap; leave the hard cap as inherited (RLIM_INFINITY) */
	struct rlimit rl;
	if (getrlimit(RLIMIT_NPROC, &rl) != 0) { printf("fail getrlimit\n"); return 1; }
	rl.rlim_cur = CAP;
	if (setrlimit(RLIMIT_NPROC, &rl) != 0) { printf("fail setrlimit\n"); return 1; }

	pid_t pids[64];
	int saw = 0;
	int n = fork_until_eagain(pids, 64, &saw);

	if (!saw) { printf("fail no-eagain n=%d\n", n); return 1; }
	/* this process counts toward the cap, so children must be < CAP */
	if (n >= CAP) { printf("fail too-many n=%d\n", n); return 1; }

	for (int i = 0; i < n; i++) { kill(pids[i], SIGKILL); waitpid(pids[i], NULL, 0); }

	/* count recovered: one more fork must now succeed */
	pid_t c = fork();
	if (c == 0) _exit(0);
	if (c < 0) { printf("fail no-recover\n"); return 1; }
	waitpid(c, NULL, 0);

	printf("OK\n");
	return 0;
}

int main(void)
{
	pid_t child = fork();
	if (child == 0) {
		if (setgid(1000) != 0 || setuid(1000) != 0) { printf("fail drop-priv\n"); _exit(1); }
		_exit(bounded_body());
	}
	if (child < 0) { printf("fail fork\n"); return 0; }

	int st = 0;
	waitpid(child, &st, 0);
	if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
		/* the child already printed the specific failure */
		return 0;
	}

	/* root is exempt: even with a low RLIMIT_NPROC set, root forks past it */
	struct rlimit rl;
	if (getrlimit(RLIMIT_NPROC, &rl) != 0) { printf("fail root-getrlimit\n"); return 0; }
	rl.rlim_cur = 2;
	if (setrlimit(RLIMIT_NPROC, &rl) != 0) { printf("fail root-setrlimit\n"); return 0; }
	pid_t r[6]; int rn = 0; int saw = 0;
	while (rn < 6) {
		pid_t c = fork();
		if (c == 0) _exit(0);
		if (c < 0) { saw = 1; break; }
		r[rn++] = c;
	}
	for (int i = 0; i < rn; i++) waitpid(r[i], NULL, 0);
	if (saw) { printf("fail root-capped\n"); return 0; }

	printf("OK\n");
	return 0;
}
