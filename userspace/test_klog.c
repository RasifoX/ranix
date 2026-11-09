/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* The kernel log: a call site that keeps firing is cut off after a burst, the rest
 * is summed up in one line when its window ends, and a debug line lands in the ring
 * even though it never reaches the console. First failure or OK. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

#define NOSYS 4000   /* no such syscall: the kernel logs each one */
#define BURST 10

static char log[1 << 19];

static int count(const char *needle)
{
	int fd = open("/dev/kmsg", O_RDONLY | O_NONBLOCK);
	if (fd < 0) { puts("FAIL kmsg"); exit(1); }
	size_t have = 0;
	ssize_t r;
	while (have < sizeof(log) - 1 && (r = read(fd, log + have, sizeof(log) - 1 - have)) > 0)
		have += (size_t)r;
	close(fd);
	log[have] = 0;
	int n = 0;
	for (const char *p = log; (p = strstr(p, needle)); p += strlen(needle)) n++;
	return n;
}

int main(void)
{
	if (count("initrd: dump:") != 1) { puts("FAIL debug line not in the ring"); return 1; }
	int base = count("syscall: unimplemented 4000");
	for (int i = 0; i < 50; i++) syscall(NOSYS);
	int got = count("syscall: unimplemented 4000") - base;
	if (got < 1 || got > BURST) { printf("FAIL %d lines through\n", got); return 1; }
	sleep(6);
	syscall(NOSYS);
	int sum = count("similar lines suppressed: syscall: unimplemented");
	if (sum < 1) { puts("FAIL no summary"); return 1; }
	puts("OK");
	return 0;
}
