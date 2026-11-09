/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* Below vm.mmap_min_addr (64 KB) an unprivileged process maps nothing, so a
 * kernel NULL dereference can never land on user memory it planted there.
 * MAP_FIXED and MAP_FIXED_NOREPLACE are EPERM, as is mremap there; root, with
 * CAP_SYS_RAWIO, may, as on Linux. /proc/sys/vm/mmap_min_addr says 65536. */
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

static int low_map_errno(int flags)
{
	void *p = mmap((void *)0x1000, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | flags, -1, 0);
	if (p == MAP_FAILED) return errno;
	munmap(p, 4096);
	return 0;
}

static int unpriv(void)
{
	if (geteuid() == 0 && (setgid(65534) || setuid(65534))) return 100;
	int bad = 0;
	if (low_map_errno(MAP_FIXED) != EPERM) bad |= 1;
	if (low_map_errno(MAP_FIXED_NOREPLACE) != EPERM) bad |= 2;
	void *m = mmap(NULL, 4096, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (mremap(m, 4096, 4096, MREMAP_MAYMOVE | MREMAP_FIXED, (void *)0x2000) != MAP_FAILED || errno != EPERM) bad |= 4;
	return bad;
}

int main(void)
{
	int bad = 0;
	pid_t k = fork();
	if (k == 0) _exit(unpriv());
	int st = 0;
	waitpid(k, &st, 0);
	int r = WIFEXITED(st) ? WEXITSTATUS(st) : 99;
	if (r & 1) { puts("unprivileged mmap(0x1000, MAP_FIXED): not EPERM"); bad++; }
	if (r & 2) { puts("unprivileged MAP_FIXED_NOREPLACE at 0x1000: not EPERM"); bad++; }
	if (r & 4) { puts("unprivileged mremap to 0x2000: not EPERM"); bad++; }
	if (r >= 8) { printf("child status %d\n", r); bad++; }
	if (geteuid() == 0 && low_map_errno(MAP_FIXED) != 0) { puts("root (CAP_SYS_RAWIO) cannot map at 0x1000"); bad++; }
	FILE *f = fopen("/proc/sys/vm/mmap_min_addr", "r");
	long v = -1;
	if (!f || fscanf(f, "%ld", &v) != 1 || v != 65536) { printf("/proc/sys/vm/mmap_min_addr: %ld\n", v); bad++; }
	if (f) fclose(f);
	puts(bad ? "FAIL" : "OK");
	return bad != 0;
}
