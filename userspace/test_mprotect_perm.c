/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* mprotect may not grant what mmap would have refused:
 *  - a file on a noexec mount cannot be mapped PROT_EXEC, so a read-only
 *    mapping of it must not become executable through mprotect either
 *    (otherwise noexec is one syscall away from useless). Linux says EPERM
 *    for the mmap and EACCES for the mprotect;
 *  - a MAP_SHARED mapping of an O_RDONLY fd must not become writable;
 *  - an unaligned address is EINVAL, a range with a hole is ENOMEM.
 * Prints FAIL lines, then OK or FAIL <n>. Runs as root (mounts a tmpfs). */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>

#define DIR_  "/tmp/mprot_nx"
#define FILE_ DIR_ "/blob"

static int fails;
#define CHECK(c, what) do { if (!(c)) { printf("FAIL %s (errno=%d)\n", what, errno); fails++; } } while (0)

int main(void)
{
	if (geteuid() != 0) { puts("FAIL run as root"); return 0; }
	long pg = sysconf(_SC_PAGESIZE);
	mkdir(DIR_, 0755);
	if (mount("tmpfs", DIR_, "tmpfs", MS_NOEXEC, NULL) != 0) { puts("FAIL mount noexec tmpfs"); return 0; }

	char buf[4096];
	memset(buf, 0xc3, sizeof(buf));                  /* x86 'ret' bytes, never run */
	int fd = open(FILE_, O_CREAT | O_RDWR | O_TRUNC, 0755);
	if (fd < 0 || write(fd, buf, sizeof(buf)) != (ssize_t)sizeof(buf)) { puts("FAIL setup file"); goto out; }
	close(fd);

	fd = open(FILE_, O_RDONLY);
	if (fd < 0) { puts("FAIL reopen"); goto out; }

	/* noexec: mmap refuses PROT_EXEC, mprotect must refuse it too */
	errno = 0;
	void *x = mmap(NULL, (size_t)pg, PROT_READ | PROT_EXEC, MAP_PRIVATE, fd, 0);
	CHECK(x == MAP_FAILED && errno == EPERM, "mmap PROT_EXEC of a noexec file allowed");
	if (x != MAP_FAILED) munmap(x, (size_t)pg);
	void *r = mmap(NULL, (size_t)pg, PROT_READ, MAP_PRIVATE, fd, 0);
	CHECK(r != MAP_FAILED, "mmap PROT_READ of a noexec file");
	if (r != MAP_FAILED) {
		errno = 0;
		CHECK(mprotect(r, (size_t)pg, PROT_READ | PROT_EXEC) == -1 && errno == EACCES,
		      "mprotect added PROT_EXEC to a noexec file mapping");
		munmap(r, (size_t)pg);
	}

	/* read-only fd, shared mapping: no write through mprotect */
	void *s = mmap(NULL, (size_t)pg, PROT_READ, MAP_SHARED, fd, 0);
	CHECK(s != MAP_FAILED, "mmap MAP_SHARED PROT_READ of an O_RDONLY fd");
	if (s != MAP_FAILED) {
		errno = 0;
		CHECK(mprotect(s, (size_t)pg, PROT_READ | PROT_WRITE) == -1 && errno == EACCES,
		      "mprotect added PROT_WRITE to a shared mapping of an O_RDONLY fd");
		munmap(s, (size_t)pg);
	}
	close(fd);

	/* argument checks */
	char *a = mmap(NULL, (size_t)pg * 3, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (a == MAP_FAILED) { puts("FAIL anon mmap"); goto out; }
	errno = 0;
	/* the raw call: musl's mprotect() rounds the address down itself */
	CHECK(syscall(SYS_mprotect, a + 1, (size_t)pg, PROT_READ) == -1 && errno == EINVAL,
	      "mprotect of an unaligned address");
	munmap(a + pg, (size_t)pg);                      /* punch a hole in the middle */
	errno = 0;
	CHECK(mprotect(a, (size_t)pg * 3, PROT_READ) == -1 && errno == ENOMEM, "mprotect across a hole");
	errno = 0;
	CHECK(mprotect(a, (size_t)pg, 0x80) == -1 && errno == EINVAL, "mprotect with an unknown prot bit");
	munmap(a, (size_t)pg * 3);

out:
	unlink(FILE_);
	umount(DIR_);
	rmdir(DIR_);
	if (fails) printf("FAIL %d\n", fails); else puts("OK");
	return 0;
}
