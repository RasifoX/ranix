/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* The boot screen's switch, /proc/sys/kernel/ranix_splash, and what a classic
 * (not quiet) boot shows. The suite boots without "quiet", so:
 *  - the file reads "off 0", and every word is taken but changes nothing;
 *  - a number above 100, garbage and an empty write are EINVAL;
 *  - a user who is not root gets EPERM;
 *  - "<3>" at the start of a /dev/kmsg write is a level: the text after it is
 *    logged without the prefix (Linux semantics).
 * The screens themselves (quiet and classic, Esc, errors, shutdown, the font)
 * are tests/bootscreen.py, which boots QEMU four times. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/klog.h>
#include <sys/wait.h>
#include <unistd.h>

#define P "/proc/sys/kernel/ranix_splash"
#define FAIL(...) do { printf("FAIL " __VA_ARGS__); printf("\n"); return 1; } while (0)

static int rd(char *b, size_t cap)
{
	int fd = open(P, O_RDONLY);
	if (fd < 0) return -errno;
	int n = (int)read(fd, b, cap - 1);
	close(fd);
	if (n < 0) return -1;
	b[n] = 0;
	return n;
}

static int wr(const char *s)
{
	int fd = open(P, O_WRONLY);
	if (fd < 0) return -errno;
	int r = (int)write(fd, s, strlen(s));
	int e = errno;
	close(fd);
	return r < 0 ? -e : 0;
}

static char logbuf[1 << 18];

int main(void)
{
	char b[64];
	if (rd(b, sizeof b) <= 0) FAIL("read %s", P);
	if (strcmp(b, "off 0\n")) FAIL("classic boot reads '%s'", b);
	static const char *const words[] = { "50", "100\n", "done", "log\n", "poweroff", "reboot" };
	for (size_t i = 0; i < sizeof words / sizeof words[0]; i++)
		if (wr(words[i])) FAIL("'%s' refused", words[i]);
	if (rd(b, sizeof b) <= 0 || strcmp(b, "off 0\n")) FAIL("classic boot changed: '%s'", b);
	static const char *const bad[] = { "101", "abc", "", "\n", "1234", "5x", "done please" };
	for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++)
		if (wr(bad[i]) != -EINVAL) FAIL("'%s' not EINVAL", bad[i]);
	pid_t p = fork();
	if (p == 0) {
		if (setgid(65534) || setuid(65534)) _exit(3);
		int r = wr("log");
		_exit(r == -EPERM || r == -EACCES ? 0 : 4);
	}
	int st = 0;
	waitpid(p, &st, 0);
	if (!WIFEXITED(st) || WEXITSTATUS(st)) FAIL("unprivileged write st=%d", st);

	int fd = open("/dev/kmsg", O_WRONLY);
	if (fd < 0) FAIL("open /dev/kmsg");
	const char *m = "<3>splashtest: an error line\n";
	if (write(fd, m, strlen(m)) != (ssize_t)strlen(m)) FAIL("kmsg write");
	close(fd);
	int n = klogctl(3, logbuf, sizeof logbuf - 1);
	if (n <= 0) FAIL("klogctl");
	logbuf[n] = 0;
	if (!strstr(logbuf, "] splashtest: an error line")) FAIL("kmsg line not logged as text");
	if (strstr(logbuf, "<3>splashtest")) FAIL("level prefix kept in the log");
	printf("OK\n");
	return 0;
}
