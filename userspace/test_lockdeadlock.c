/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* F_SETLKW that would close a wait cycle fails with EDEADLK instead of
 * sleeping forever (LTP fcntl17): the parent holds byte 0, a child holds
 * byte 1 and waits for byte 0; the parent asking for byte 1 is a deadlock.
 * The child still gets byte 0 once the parent lets go. */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int lk(int fd, int cmd, int type, off_t at)
{
	struct flock fl = { .l_type = type, .l_whence = SEEK_SET, .l_start = at, .l_len = 1 };
	return fcntl(fd, cmd, &fl);
}

int main(void)
{
	int bad = 0;
	alarm(10);   /* a missed deadlock must fail, not hang */
	int fd = open("/tmp/lockdeadlock", O_CREAT | O_RDWR | O_TRUNC, 0644);
	if (fd < 0 || write(fd, "xy", 2) != 2) { puts("setup failed"); return 1; }
	if (lk(fd, F_SETLK, F_WRLCK, 0)) { puts("lock byte 0 failed"); return 1; }
	int p[2];
	pipe(p);
	pid_t c = fork();
	if (c == 0) {
		if (lk(fd, F_SETLK, F_WRLCK, 1)) _exit(2);
		write(p[1], "r", 1);
		if (lk(fd, F_SETLKW, F_WRLCK, 0)) _exit(3);   /* waits for the parent */
		_exit(0);
	}
	char x;
	read(p[0], &x, 1);
	struct timespec ts = { 0, 300 * 1000 * 1000 };
	nanosleep(&ts, NULL);   /* let the child block on byte 0 */
	errno = 0;
	int r = lk(fd, F_SETLKW, F_WRLCK, 1);
	if (r != -1 || errno != EDEADLK) { printf("closing the cycle: %d errno %d, want EDEADLK\n", r, errno); bad++; }
	lk(fd, F_SETLK, F_UNLCK, 0);
	int st;
	waitpid(c, &st, 0);
	if (!WIFEXITED(st) || WEXITSTATUS(st)) { printf("child: status 0x%x, want it to get byte 0\n", st); bad++; }
	unlink("/tmp/lockdeadlock");
	puts(bad ? "FAIL" : "OK");
	return bad != 0;
}
