/* pid-namespace procfs view: a process in a new CLONE_NEWPID namespace sees
 * only the tasks of that namespace under /proc, named by their namespace-local
 * pid. The first task is pid 1, and no host process leaks in — so /proc lists
 * exactly one numeric entry ("1"), and /proc/self resolves to it. */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sched.h>
#include <sys/syscall.h>
#include <sys/wait.h>

static int numeric(const char *s)
{
	if (!*s) return 0;
	for (const char *p = s; *p; p++) if (*p < '0' || *p > '9') return 0;
	return 1;
}

int main(void)
{
	long pid = syscall(SYS_clone, CLONE_NEWPID | 17 /*SIGCHLD*/, 0, 0, 0, 0);
	if (pid == 0) {
		if (getpid() != 1) _exit(3);

		DIR *d = opendir("/proc");
		if (!d) _exit(4);
		int n_numeric = 0, saw_one = 0, saw_other = 0;
		struct dirent *e;
		while ((e = readdir(d))) {
			if (!numeric(e->d_name)) continue;
			n_numeric++;
			if (strcmp(e->d_name, "1") == 0) saw_one = 1;
			else saw_other = 1;   /* a leaked host pid */
		}
		closedir(d);

		char buf[64];
		long r = readlink("/proc/self", buf, sizeof(buf) - 1);
		int self_is_1 = (r > 0 && (buf[r] = 0, strcmp(buf, "/proc/1") == 0));

		_exit((saw_one && !saw_other && n_numeric == 1 && self_is_1) ? 0 : 5);
	}

	int st;
	waitpid((pid_t)pid, &st, 0);
	if (WIFEXITED(st) && WEXITSTATUS(st) == 0)
		printf("OK\n");
	else
		printf("pidnsproc-fail code=%d\n", WIFEXITED(st) ? WEXITSTATUS(st) : -1);
	return 0;
}
