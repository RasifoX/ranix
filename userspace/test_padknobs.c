/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* /sys/kernel/touchpad takes the touchpad's choices one at a time: "tap 0|1",
 * "tap3 0|1" (three fingers: middle button), "middle 0|1" (both buttons: middle).
 * Anything else is EINVAL. Reading the file gives, after the pad's raw report,
 * a line of the choices in the same words, so a write can be checked. Left on. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int put(const char *s)
{
	int fd = open("/sys/kernel/touchpad", O_WRONLY);
	if (fd < 0) return -errno;
	int r = write(fd, s, strlen(s)) == (ssize_t)strlen(s) ? 0 : -errno;
	close(fd);
	return r;
}

int main(void)
{
	static const char *const ok[] = {"tap3 0", "tap3 1", "middle 0", "middle 1", "tap 1", "tap3 1\n"};
	static const char *const bad[] = {"tap3 2", "bogus 1", "middle", "tap3x 1"};
	for (unsigned i = 0; i < sizeof ok / sizeof ok[0]; i++)
		if (put(ok[i])) { printf("FAIL '%s' %d\n", ok[i], put(ok[i])); return 1; }
	for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++)
		if (put(bad[i]) != -EINVAL) { printf("FAIL '%s' accepted\n", bad[i]); return 1; }
	static const struct { const char *w, *line; } rd[] = {
		{"tap3 0", "tap 1 tap3 0 middle 1\n"}, {"middle 0", "tap 1 tap3 0 middle 0\n"},
		{"tap 0", "tap 0 tap3 0 middle 0\n"}, {"tap 1", "tap 1 tap3 0 middle 0\n"},
		{"tap3 1", "tap 1 tap3 1 middle 0\n"}, {"middle 1", "tap 1 tap3 1 middle 1\n"},
	};
	for (unsigned i = 0; i < sizeof rd / sizeof rd[0]; i++) {
		char b[512];
		if (put(rd[i].w)) { printf("FAIL '%s'\n", rd[i].w); return 1; }
		int fd = open("/sys/kernel/touchpad", O_RDONLY);
		ssize_t n = fd >= 0 ? read(fd, b, sizeof b - 1) : -1;
		if (fd >= 0) close(fd);
		b[n > 0 ? n : 0] = 0;
		if (!strstr(b, rd[i].line)) { printf("FAIL after '%s' read '%s'\n", rd[i].w, b); return 1; }
	}
	puts("OK");
	return 0;
}
