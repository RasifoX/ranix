/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* writev of a short record to a pipe is one atomic write (POSIX, Linux): a
 * reader that takes one read and closes its end has the whole record, and the
 * writer, which wrote nothing after it, must not die of SIGPIPE. musl's printf
 * ends a line with a two-iovec writev, so a pipe reader of `prog | x` hit this.
 * Each round: child writev("0 0 1", "\n"), parent one read, close, wait.
 * Prints OK, or FAIL with the first bad round. */
#include <stdio.h>
#include <string.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

#define ROUNDS 400

int main(void)
{
	for (int i = 0; i < ROUNDS; i++) {
		int pfd[2];
		if (pipe(pfd)) { puts("FAIL pipe"); return 0; }
		pid_t c = fork();
		if (c == 0) {
			close(pfd[0]);
			struct iovec v[2] = { { "0 0 1", 5 }, { "\n", 1 } };
			_exit(writev(pfd[1], v, 2) == 6 ? 0 : 1);
		}
		close(pfd[1]);
		char buf[64];
		ssize_t n = read(pfd[0], buf, sizeof buf);
		close(pfd[0]);
		int st = 0;
		waitpid(c, &st, 0);
		if (n != 6 || memcmp(buf, "0 0 1\n", 6)) {
			printf("FAIL round %d: one read got %zd bytes\n", i, n); return 0;
		}
		if (!WIFEXITED(st) || WEXITSTATUS(st)) {
			printf("FAIL round %d: writer status 0x%x (signal %d)\n", i, st,
			       WIFSIGNALED(st) ? WTERMSIG(st) : 0);
			return 0;
		}
	}
	puts("OK");
	return 0;
}
