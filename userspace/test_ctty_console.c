/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* /dev/tty with no controlling terminal, as Linux has it: ENXIO.
 * The console's line holds what is typed at the machine (the lock screen's
 * password too). Ranix handed it to any process with no terminal that was not a
 * session leader, so a program another person left running (setsid from a
 * terminal, then a child) read the keys and could switch the screen to text
 * over the lock (KDSETMODE). Root keeps the old way (init's shell has no
 * terminal); the console nodes stay root's. A process with its own terminal (a
 * pty) still opens /dev/tty, as that terminal.
 * Runs as root. Prints FAIL lines, then OK or FAIL. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <grp.h>
#include <sys/ioctl.h>
#include <sys/wait.h>

#define NOBODY 65534
#define KDSETMODE 0x4B3A
#define KD_TEXT 0
#define VT_ACTIVATE 0x5606

/* the errno of opening path, 0 when it opened */
static int try_open(const char *path, int fl)
{
	int fd = open(path, fl | O_NOCTTY | O_CLOEXEC);
	if (fd < 0) return errno;
	close(fd);
	return 0;
}

/* in a fresh session with no terminal, a child that is not its leader:
 * uid 0 stays root; else it becomes NOBODY. Exit 0 = every check passed. */
static int no_tty(int as_root)
{
	pid_t p = fork();
	if (p < 0) return 1;
	if (p == 0) {
		if (setsid() < 0) _exit(10);
		pid_t q = fork();
		if (q < 0) _exit(11);
		if (q == 0) {
			int bad = 0;
			if (getsid(0) == getpid()) { printf("FAIL the child leads its session\n"); _exit(12); }
			if (!as_root && (setgroups(0, NULL) != 0 || setgid(NOBODY) != 0 || setuid(NOBODY) != 0)) _exit(13);
			int e = try_open("/dev/tty", O_RDWR);
			if (as_root && e) { printf("FAIL root, no terminal: /dev/tty errno %d (the console expected)\n", e); bad++; }
			if (!as_root) {
				if (e != ENXIO) { printf("FAIL uid %d, no terminal: /dev/tty errno %d, ENXIO expected\n", NOBODY, e); bad++; }
				if (e == 0) {
					int fd = open("/dev/tty", O_RDONLY | O_NOCTTY | O_NONBLOCK);
					if (fd >= 0 && ioctl(fd, KDSETMODE, (long)KD_TEXT) == 0)
						printf("FAIL uid %d switched the console to text\n", NOBODY), bad++;
					if (fd >= 0) close(fd);
				}
				static const char *const nodes[] = { "/dev/console", "/dev/tty0", "/dev/tty1", "/dev/tty6", NULL };
				for (int i = 0; nodes[i]; i++) {
					int ne = try_open(nodes[i], O_RDONLY);
					if (ne != EACCES) { printf("FAIL uid %d: %s errno %d, EACCES expected\n", NOBODY, nodes[i], ne); bad++; }
				}
			}
			_exit(bad ? 1 : 0);
		}
		int st;
		if (waitpid(q, &st, 0) != q) _exit(14);
		_exit(WIFEXITED(st) ? WEXITSTATUS(st) : 15);
	}
	int st;
	if (waitpid(p, &st, 0) != p) return 1;
	return WIFEXITED(st) ? WEXITSTATUS(st) : 1;
}

/* a session with a pty as its terminal: /dev/tty opens, as that pty, also after setuid */
static int own_pty(void)
{
	int m = posix_openpt(O_RDWR | O_NOCTTY);
	if (m < 0 || grantpt(m) != 0 || unlockpt(m) != 0) { printf("FAIL no pty: errno %d\n", errno); return 1; }
	char name[64];
	if (ptsname_r(m, name, sizeof name) != 0) { printf("FAIL ptsname\n"); return 1; }
	pid_t p = fork();
	if (p < 0) return 1;
	if (p == 0) {
		if (setsid() < 0) _exit(20);
		int s = open(name, O_RDWR);
		if (s < 0) _exit(21);
		ioctl(s, TIOCSCTTY, 0);
		pid_t q = fork();
		if (q == 0) {
			if (setgroups(0, NULL) != 0 || setgid(NOBODY) != 0 || setuid(NOBODY) != 0) _exit(22);
			int e = try_open("/dev/tty", O_RDWR);
			if (e) { printf("FAIL uid %d with a pty: /dev/tty errno %d\n", NOBODY, e); _exit(1); }
			_exit(0);
		}
		int st;
		if (q < 0 || waitpid(q, &st, 0) != q) _exit(23);
		_exit(WIFEXITED(st) ? WEXITSTATUS(st) : 24);
	}
	int st;
	if (waitpid(p, &st, 0) != p) return 1;
	close(m);
	return WIFEXITED(st) ? WEXITSTATUS(st) : 1;
}

int main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	if (geteuid() != 0) { puts("FAIL not root"); return 1; }
	int r1 = no_tty(1), r2 = no_tty(0), r3 = own_pty();
	if (r1) printf("FAIL root without a terminal (%d)\n", r1);
	if (r2) printf("FAIL another user without a terminal (%d)\n", r2);
	if (r3) printf("FAIL a user with a pty (%d)\n", r3);
	/* VT switching: one console here, no VTs; asked of the console it is refused, never a switch */
	int fd = open("/dev/tty0", O_RDWR | O_NOCTTY | O_CLOEXEC);
	if (fd >= 0) {
		if (ioctl(fd, VT_ACTIVATE, 2L) == 0) { puts("FAIL VT_ACTIVATE taken"); r1 = 1; }
		close(fd);
	}
	puts(r1 || r2 || r3 ? "FAIL" : "OK");
	return r1 || r2 || r3;
}
