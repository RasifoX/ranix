/* exec with a large argv (well past the old 64-arg / 16 KB caps) and long
 * paths (past the old 256 limit, up to PATH_MAX). The child re-execs itself in
 * "count" mode, sums argv, and reports argc + checksum back via exit status so
 * the parent can verify nothing was dropped or truncated. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/auxv.h>

#define NARGS 2000   /* > 64, total bytes > 16 KB */
#define REPORT "/tmp/execargs.report"

extern char **environ;

/* child "count" mode: argv = { prog, "count", a0, a1, ... }. Writes the payload
 * arg count and a rolling checksum to REPORT so the parent can compare. */
static int count_mode(int argc, char **argv)
{
	int n = argc - 2;                 /* payload args after prog + "count" */
	unsigned sum = 0;
	for (int i = 2; i < argc; i++)
		for (char *s = argv[i]; *s; s++)
			sum = sum * 31u + (unsigned char)*s;
	FILE *f = fopen(REPORT, "w");
	if (!f) _exit(1);
	fprintf(f, "%d %u\n", n, sum);
	fclose(f);
	_exit(0);
}

static int run_big_argv(const char *self)
{
	char **av = malloc((NARGS + 3) * sizeof(char *));
	av[0] = (char *)self;
	av[1] = "count";
	unsigned expect_sum = 0;
	char *store = malloc(NARGS * 8);
	for (int i = 0; i < NARGS; i++) {
		char *s = store + i * 8;
		snprintf(s, 8, "%d", i);
		av[2 + i] = s;
		for (char *p = s; *p; p++)
			expect_sum = expect_sum * 31u + (unsigned char)*p;
	}
	av[2 + NARGS] = 0;

	unlink(REPORT);
	pid_t pid = fork();
	if (pid == 0) {
		execve(self, av, environ);
		_exit(127);
	}
	int st = 0;
	waitpid(pid, &st, 0);
	if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) return 0;

	FILE *f = fopen(REPORT, "r");
	if (!f) return 0;
	int got_n = -1; unsigned got_sum = 0;
	int r = fscanf(f, "%d %u", &got_n, &got_sum);
	fclose(f);
	return r == 2 && got_n == NARGS && got_sum == expect_sum;
}

static int run_long_path(void)
{
	/* build a directory chain whose full path exceeds 256 bytes but is well
	 * under PATH_MAX, then create/stat/open a file there. */
	char path[1024];
	int n = 0;
	path[n++] = '/'; path[n++] = 't'; path[n++] = 'm'; path[n++] = 'p';
	/* 16 components of "/deepdirectory_levelNN" (~22 each) ≈ 350 > 256 */
	for (int d = 0; d < 16; d++) {
		n += snprintf(path + n, sizeof(path) - n, "/deepdirectory_level%02d", d);
		if (mkdir(path, 0777) != 0 && errno != EEXIST) return 0;
	}
	if ((int)strlen(path) <= 256) return 0;   /* must actually exceed old limit */
	n += snprintf(path + n, sizeof(path) - n, "/file.txt");

	int fd = open(path, O_CREAT | O_WRONLY, 0644);
	if (fd < 0) return 0;
	if (write(fd, "hello", 5) != 5) { close(fd); return 0; }
	close(fd);

	struct stat sb;
	if (stat(path, &sb) != 0 || sb.st_size != 5) return 0;
	return 1;
}

static int run_too_long_path(void)
{
	char path[PATH_MAX + 64];
	memset(path, 'a', sizeof(path) - 1);
	path[0] = '/';
	path[sizeof(path) - 1] = '\0';
	errno = 0;
	int fd = open(path, O_RDONLY);
	if (fd >= 0) { close(fd); return 0; }
	return errno == ENAMETOOLONG;
}

/* AT_RANDOM must point at 16 live, non-zero seed bytes — the stack canary and
 * pointer guard derive from it. A zeroed (uninitialised) block means the kernel
 * never filled the slot it reserved. */
static int run_at_random(void)
{
	unsigned long ar = getauxval(AT_RANDOM);
	if (!ar) return 0;
	const unsigned char *r = (const unsigned char *)ar;
	int nz = 0;
	for (int i = 0; i < 16; i++) if (r[i]) nz++;
	return nz != 0;
}

int main(int argc, char **argv)
{
	if (argc >= 2 && strcmp(argv[1], "count") == 0)
		return count_mode(argc, argv);

	const char *self = "/bin/test_execargs";
	int ok_argv = run_big_argv(self);
	int ok_long = run_long_path();
	int ok_2big = run_too_long_path();
	int ok_rand = run_at_random();

	if (ok_argv && ok_long && ok_2big && ok_rand) puts("OK");
	else printf("FAIL argv=%d long=%d toolong=%d rand=%d\n",
	            ok_argv, ok_long, ok_2big, ok_rand);
	return 0;
}
