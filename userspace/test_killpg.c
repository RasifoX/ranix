/* kill(2) process-group / broadcast semantics:
 *   kill(-pgrp,sig) and killpg() deliver to every member of the group;
 *   kill(0,sig) delivers to the caller's group;
 *   kill(pid,0) is an existence probe.
 * The child puts itself in its own group, forks two grandchildren into that
 * group, then SIGTERMs the group with kill(-pgrp) and counts the deaths. */
#define _GNU_SOURCE
#include <stdio.h>
#include <signal.h>
#include <unistd.h>
#include <sys/wait.h>
#include <errno.h>

int main(void)
{
	/* existence probe of a definitely-absent pid */
	if (kill(0x7ffffff0, 0) == 0 || errno != ESRCH) { printf("killpg-fail probe\n"); return 0; }
	/* probe of self must succeed */
	if (kill(getpid(), 0) != 0) { printf("killpg-fail self-probe\n"); return 0; }

	pid_t leader = fork();
	if (leader == 0) {
		setpgid(0, 0);                 /* new group, leader == self */
		for (int i = 0; i < 2; i++) {
			pid_t g = fork();
			if (g == 0) { for (;;) pause(); }  /* member, waits for the signal */
		}
		signal(SIGTERM, SIG_IGN);      /* the leader survives its own broadcast */
		/* give children time to be scheduled into pause() */
		for (volatile int s = 0; s < 2000000; s++) {}
		if (killpg(getpgrp(), SIGTERM) != 0) _exit(2);
		int reaped = 0, st;
		while (waitpid(-1, &st, 0) > 0) reaped++;
		_exit(reaped == 2 ? 0 : 3);    /* both grandchildren must have died */
	}

	int st;
	waitpid(leader, &st, 0);
	if (WIFEXITED(st) && WEXITSTATUS(st) == 0) printf("OK\n");
	else printf("killpg-fail exit=%d\n", WIFEXITED(st) ? WEXITSTATUS(st) : -1);
	return 0;
}
