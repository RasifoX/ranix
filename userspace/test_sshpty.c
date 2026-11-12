/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* "ssh -tt HOST CMD" from a terminal: run the given client command line on a
 * fresh pty (forkpty, as a terminal emulator does) and print what it showed,
 * without the CRs. dbclient -t needs its stdin to be a terminal (it puts it in
 * raw mode), so the suite cannot just pipe into it. With -i TEXT, TEXT is typed
 * on that terminal once a second has passed (\r ends a line, as Enter does).
 *     test_sshpty dbclient -t -t ... 'echo A; sh -c "echo B"; echo C'
 *     test_sshpty -i 'hello\r' dbclient -t -t ... 'read x; echo "got:$x"'   */
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <pty.h>
#include <poll.h>
#include <string.h>
#include <time.h>
#include <sys/wait.h>

int main(int argc, char **argv)
{
	const char *input = NULL;
	if (argc > 2 && strcmp(argv[1], "-i") == 0) { input = argv[2]; argv += 2; argc -= 2; }
	if (argc < 2) { fprintf(stderr, "usage: test_sshpty [-i TEXT] PROG [ARGS...]\n"); return 2; }
	int m;
	pid_t pid = forkpty(&m, NULL, NULL, NULL);
	if (pid < 0) { perror("forkpty"); return 1; }
	if (pid == 0) {
		execvp(argv[1], argv + 1);
		_exit(127);
	}
	alarm(25);
	char buf[512];
	struct timespec t0, t;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (;;) {
		if (input) {
			struct pollfd pf = { m, POLLIN, 0 };
			if (poll(&pf, 1, 100) == 0) {
				clock_gettime(CLOCK_MONOTONIC, &t);
				if (t.tv_sec - t0.tv_sec >= 1) {
					char in[256]; size_t k = 0;   /* "\r" in the argument is Enter */
					for (const char *c = input; *c && k < sizeof(in); c++) {
						if (c[0] == '\\' && c[1] == 'r') { in[k++] = '\r'; c++; }
						else in[k++] = *c;
					}
					write(m, in, k);
					input = NULL;
				}
				continue;
			}
		}
		ssize_t n = read(m, buf, sizeof(buf));
		if (n < 0 && errno == EINTR) continue;
		if (n <= 0) break;              /* the client is gone: EOF or EIO */
		for (ssize_t i = 0; i < n; i++)
			if (buf[i] != '\r') putchar(buf[i]);
	}
	fflush(stdout);
	int st = 0;
	waitpid(pid, &st, 0);
	close(m);
	return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
}
