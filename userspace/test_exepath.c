/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* /proc/self/exe after exec through a link: it must name the real file, not
 * the link. Chromium (VS Code) starts its helpers as execv("/proc/self/exe"),
 * and ld.so takes $ORIGIN from /proc/self/exe; "/proc/self" there meant
 * "libffmpeg.so: cannot open shared object file". */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

int main(int argc, char **argv)
{
	char me[512], got[512];
	ssize_t n = readlink("/proc/self/exe", me, sizeof me - 1);
	if (n <= 0) { printf("FAIL readlink\n"); return 0; }
	me[n] = 0;
	if (argc > 1 && !strcmp(argv[1], "child")) { printf("%s", me); fflush(stdout); return 0; }
	int pp[2];
	if (pipe(pp)) { printf("FAIL pipe\n"); return 0; }
	pid_t c = fork();
	if (c == 0) {
		dup2(pp[1], 1);
		close(pp[0]);
		execl("/proc/self/exe", "x", "child", (char *)0);
		_exit(9);
	}
	close(pp[1]);
	ssize_t k = read(pp[0], got, sizeof got - 1);
	got[k > 0 ? k : 0] = 0;
	int st;
	waitpid(c, &st, 0);
	if (strcmp(got, me)) { printf("FAIL exe via /proc/self/exe is '%s', want '%s'\n", got, me); return 0; }
	if (!strncmp(me, "/proc/", 6)) { printf("FAIL exe is a proc path '%s'\n", me); return 0; }
	printf("OK\n");
	return 0;
}
