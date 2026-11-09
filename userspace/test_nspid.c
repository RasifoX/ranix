/* /proc/<pid>/status carries NStgid and NSpid (Linux): the task's number in each
 * pid namespace from the reader's down to its own. A child that is pid 1 of a
 * new namespace shows "<outer> 1"; a task of the reader's namespace one number.
 * Prints OK, or FAIL with what was wrong. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int line(int pid, const char *key, char *out, int cap)
{
	char p[64], b[2048];
	snprintf(p, sizeof p, "/proc/%d/status", pid);
	int fd = open(p, O_RDONLY);
	if (fd < 0) return -1;
	int n = (int)read(fd, b, sizeof b - 1);
	close(fd);
	if (n <= 0) return -1;
	b[n] = 0;
	char *s = strstr(b, key);
	if (!s) return -1;
	s += strlen(key);
	int i = 0;
	while (*s && *s != '\n' && i < cap - 1) out[i++] = *s++;
	out[i] = 0;
	return 0;
}

int main(void)
{
	char a[64], want[64];
	if (line(getpid(), "NSpid:", a, sizeof a)) { puts("FAIL no NSpid line"); return 0; }
	snprintf(want, sizeof want, "\t%d", (int)getpid());
	if (strcmp(a, want)) { printf("FAIL own NSpid '%s'\n", a); return 0; }
	if (unshare(CLONE_NEWPID)) { puts("FAIL unshare"); return 0; }
	int go[2];
	if (pipe(go)) return 0;
	pid_t c = fork();
	if (c == 0) { char x; close(go[1]); if (read(go[0], &x, 1) < 0) { } _exit(getpid() == 1 ? 0 : 1); }
	close(go[0]);
	int r1 = line(c, "NSpid:", a, sizeof a);
	char t[64];
	int r2 = line(c, "NStgid:", t, sizeof t);
	snprintf(want, sizeof want, "\t%d\t1", (int)c);
	close(go[1]);
	int st = 0;
	waitpid(c, &st, 0);
	if (r1 || strcmp(a, want)) { printf("FAIL child NSpid '%s' want '%s'\n", a, want); return 0; }
	if (r2 || strcmp(t, want)) { printf("FAIL child NStgid '%s'\n", t); return 0; }
	if (!WIFEXITED(st) || WEXITSTATUS(st)) { puts("FAIL child not pid 1 inside"); return 0; }
	puts("OK");
	return 0;
}
