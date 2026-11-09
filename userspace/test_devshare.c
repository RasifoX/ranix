/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* A MAP_SHARED mapping of device memory survives fork().
 *
 * fork() used to write-protect it like private memory, and device memory has
 * no page to copy, so the parent's next store died with SIGSEGV. The child's
 * exit also handed the frames to the page allocator.
 *
 * Needs /dev/fb0. Without one it says so on stderr and passes. */
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#define PG 4096
#define FBIOGET_FSCREENINFO 0x4602

struct fix_info {
	char id[16];
	unsigned long smem_start;
	uint32_t smem_len, type, type_aux, visual;
	uint16_t xpanstep, ypanstep, ywrapstep;
	uint32_t line_length;
	unsigned long mmio_start;
	uint32_t mmio_len, accel;
	uint16_t capabilities, reserved[2];
};

static long mem_free_kb(void)
{
	char b[256];
	long v = -1;
	int fd = open("/proc/meminfo", O_RDONLY);
	if (fd < 0) return -1;
	ssize_t n = read(fd, b, sizeof b - 1);
	close(fd);
	if (n <= 0) return -1;
	b[n] = 0;
	char *p = strstr(b, "MemFree:");
	if (p) sscanf(p + 8, "%ld", &v);
	return v;
}

static int child_status(pid_t pid)
{
	int st = 0;
	while (waitpid(pid, &st, 0) < 0) {}
	return st;
}

int main(void)
{
	int fd = open("/dev/fb0", O_RDWR);
	if (fd < 0) {            /* the default test boot has no framebuffer */
		fputs("test_devshare: no /dev/fb0, nothing tested\n", stderr);
		puts("OK");
		return 0;
	}
	struct fix_info fx;
	memset(&fx, 0, sizeof fx);
	if (ioctl(fd, FBIOGET_FSCREENINFO, &fx) != 0 || fx.smem_len < 3 * PG) {
		puts("FAIL ioctl");
		return 0;
	}
	size_t len = fx.smem_len & ~(size_t)(PG - 1);
	volatile uint32_t *m = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (m == MAP_FAILED) { puts("FAIL mmap"); return 0; }
	uint32_t keep = m[0], keep2 = m[PG / 4];

	/* The child sees the parent's store, and the parent the child's. */
	m[0] = 0x11223344;
	pid_t pid = fork();
	if (pid < 0) { puts("FAIL fork"); return 0; }
	if (pid == 0) {
		if (m[0] != 0x11223344) _exit(1);
		m[PG / 4] = 0x55667788;
		_exit(0);
	}
	int st = child_status(pid);
	if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) { puts("FAIL child store"); return 0; }
	if (m[PG / 4] != 0x55667788) { puts("FAIL not shared with the child"); return 0; }

	/* The store that used to raise SIGSEGV. */
	m[0] = 0x99aabbcc;
	if (m[0] != 0x99aabbcc) { puts("FAIL parent store"); return 0; }

	/* Mapping torn down in the child: the parent's stays good, nothing is
	 * freed, and repeating it does not grow free memory. */
	long before = mem_free_kb();
	for (int i = 0; i < 40; i++) {
		pid = fork();
		if (pid < 0) { puts("FAIL fork loop"); return 0; }
		if (pid == 0) {
			if (i & 1) munmap((void *)m, len);
			_exit(0);
		}
		child_status(pid);
		m[0] = (uint32_t)i;
	}
	long after = mem_free_kb();
	if (before >= 0 && after > before + (long)(len / 2048)) {
		printf("FAIL device frames freed: %ld to %ld kB\n", before, after);
		return 0;
	}

	/* A read-only mapping stays read-only in a child, and writable comes back. */
	if (mprotect((void *)m, len, PROT_READ) != 0) { puts("FAIL mprotect ro"); return 0; }
	pid = fork();
	if (pid == 0) {
		signal(SIGSEGV, SIG_DFL);
		m[0] = 1;
		_exit(0);
	}
	st = child_status(pid);
	if (!WIFSIGNALED(st) || WTERMSIG(st) != SIGSEGV) { puts("FAIL read-only store"); return 0; }
	if (mprotect((void *)m, len, PROT_READ | PROT_WRITE) != 0) { puts("FAIL mprotect rw"); return 0; }
	m[0] = 0x0badcafe;
	if (m[0] != 0x0badcafe) { puts("FAIL store after mprotect"); return 0; }

	/* MAP_SHARED mapped read-only first, then made writable after a fork. */
	volatile uint32_t *r = mmap(NULL, 2 * PG, PROT_READ, MAP_SHARED, fd, 0);
	if (r == MAP_FAILED) { puts("FAIL mmap ro"); return 0; }
	pid = fork();
	if (pid == 0) _exit(0);
	child_status(pid);
	if (mprotect((void *)r, 2 * PG, PROT_READ | PROT_WRITE) != 0) { puts("FAIL mprotect 2"); return 0; }
	((volatile uint32_t *)r)[1] = 0x12121212;
	if (m[1] != 0x12121212) { puts("FAIL second mapping"); return 0; }

	m[0] = keep;
	m[PG / 4] = keep2;
	puts("OK");
	return 0;
}
