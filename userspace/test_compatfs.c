/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* What chroots, debootstrap and Chromium need of the kernel:
 *  - device nodes on a disk filesystem (ext2/4): /dev/null and /dev/zero by number
 *  - mount --bind of a directory, and its umount
 *  - a 1 TB PROT_NONE reservation (V8's sandbox), unmapped in well under a second */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/utsname.h>
#include <sys/prctl.h>
#include <sys/wait.h>

static int fail(const char *w) { printf("FAIL %s errno=%d\n", w, errno); return 1; }

int main(int argc, char **argv)
{
	const char *disk = argc > 1 ? argv[1] : "/mnt";
	char p[256];
	snprintf(p, sizeof p, "%s/cfs-null", disk);
	unlink(p);
	if (mknod(p, S_IFCHR | 0666, makedev(1, 3)) != 0) return fail("mknod null");
	struct stat st;
	if (stat(p, &st) != 0 || !S_ISCHR(st.st_mode) || st.st_rdev != makedev(1, 3)) return fail("stat null");
	int fd = open(p, O_RDWR);
	if (fd < 0) return fail("open null");
	char b[16];
	if (write(fd, "x", 1) != 1 || read(fd, b, sizeof b) != 0) return fail("null io");
	close(fd);
	unlink(p);
	snprintf(p, sizeof p, "%s/cfs-zero", disk);
	unlink(p);
	if (mknod(p, S_IFCHR | 0666, makedev(1, 5)) != 0) return fail("mknod zero");
	fd = open(p, O_RDONLY);
	memset(b, 7, sizeof b);
	if (fd < 0 || read(fd, b, 8) != 8 || b[0] || b[7]) return fail("zero io");
	close(fd);
	unlink(p);

	mkdir("/tmp/cfs-src", 0755);
	mkdir("/tmp/cfs-dst", 0755);
	fd = open("/tmp/cfs-src/f", O_CREAT | O_WRONLY | O_TRUNC, 0644);
	if (fd < 0 || write(fd, "bind", 4) != 4) return fail("src file");
	close(fd);
	if (mount("/tmp/cfs-src", "/tmp/cfs-dst", NULL, MS_BIND, NULL) != 0) return fail("bind");
	fd = open("/tmp/cfs-dst/f", O_RDONLY);
	if (fd < 0 || read(fd, b, 4) != 4 || memcmp(b, "bind", 4)) return fail("bind read");
	close(fd);
	if (umount("/tmp/cfs-dst") != 0) return fail("umount bind");
	if (access("/tmp/cfs-dst/f", F_OK) == 0) return fail("bind gone");
	unlink("/tmp/cfs-src/f");
	rmdir("/tmp/cfs-src");
	rmdir("/tmp/cfs-dst");

	size_t tb = 1UL << 40;
	struct timespec t0, t1;
	void *r = mmap(NULL, tb, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
	if (r == MAP_FAILED) return fail("1 TB reserve");
	char *in = mmap((char *)r + (tb / 2), 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
	if (in == MAP_FAILED) return fail("page inside");
	in[0] = 1;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	if (munmap(r, tb) != 0) return fail("munmap 1 TB");
	clock_gettime(CLOCK_MONOTONIC, &t1);
	long ms = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;
	if (ms > 1000) { printf("FAIL munmap took %ld ms\n", ms); return 1; }
	/* each filesystem its own st_dev, matching /proc/self/mountinfo */
	struct stat r1, r2;
	if (stat("/", &r1) != 0 || stat("/proc", &r2) != 0) return fail("stat / /proc");
	if (r1.st_dev == r2.st_dev) { printf("FAIL st_dev / == /proc (%lx)\n", (unsigned long)r1.st_dev); return 1; }
	/* oom_score_adj: read, raise, read back */
	char ob[16] = {0};
	fd = open("/proc/self/oom_score_adj", O_RDWR);
	if (fd < 0 || read(fd, ob, sizeof(ob) - 1) <= 0) return fail("oom_score_adj read");
	close(fd);
	fd = open("/proc/self/oom_score_adj", O_WRONLY);
	if (fd < 0 || write(fd, "500", 3) != 3) return fail("oom_score_adj write");
	close(fd);
	memset(ob, 0, sizeof(ob));
	fd = open("/proc/self/oom_score_adj", O_RDONLY);
	if (fd < 0 || read(fd, ob, sizeof(ob) - 1) <= 0 || strncmp(ob, "500", 3)) { printf("FAIL oom_score_adj readback '%s'\n", ob); return 1; }
	close(fd);
	/* uname: Ranix by name; a tree marked for Linux software says Linux, children too */
	struct utsname u;
	if (uname(&u) != 0 || strcmp(u.sysname, "Ranix")) { printf("FAIL default sysname %s\n", u.sysname); return 1; }
	if (prctl(0x52580010, 1, 0, 0, 0) != 0) return fail("prctl uname linux");
	if (uname(&u) != 0 || strcmp(u.sysname, "Linux")) { printf("FAIL sysname after prctl %s\n", u.sysname); return 1; }
	pid_t ch = fork();
	if (ch == 0) { struct utsname cu; _exit(uname(&cu) == 0 && !strcmp(cu.sysname, "Linux") ? 0 : 1); }
	int ws = 0;
	waitpid(ch, &ws, 0);
	if (!WIFEXITED(ws) || WEXITSTATUS(ws)) return fail("child sysname");
	prctl(0x52580010, 0, 0, 0, 0);
	puts("OK");
	return 0;
}
