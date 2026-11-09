/* A dentry freed by unlink or rename gives its slot back, so churn cannot walk the
 * cache into its cap, where every lookup turns volatile. Paths through volatile
 * directories must keep resolving, with "..", chroot and a create at the end. */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>

static long dentries(void)
{
	char b[64] = "";
	int fd = open("/proc/sys/fs/dentry-state", O_RDONLY);
	if (fd < 0) return -1;
	read(fd, b, sizeof(b) - 1);
	close(fd);
	return atol(b);
}

#define FAIL(what) do { printf("FAIL %s errno %d\n", what, errno); return 1; } while (0)

int main(void)
{
	char p[64], q[64];
	mkdir("/tmp", 0755);
	mkdir("/tmp/dch", 0755);
	long before = dentries();
	if (before < 0) FAIL("dentry-state");

	for (int i = 0; i < 6000; i++) {
		snprintf(p, sizeof(p), "/tmp/dch/f%d", i);
		int fd = open(p, O_WRONLY | O_CREAT, 0644);
		if (fd < 0) { printf("FAIL create %d errno=%d\n", i, errno); return 1; }
		close(fd);
		if (i % 2) {
			snprintf(q, sizeof(q), "/tmp/dch/g%d", i);
			if (rename(p, q)) FAIL("rename");
			unlink(q);
		} else if (unlink(p)) {
			FAIL("unlink");
		}
	}
	long after = dentries();
	printf("dentries %ld -> %ld\n", before, after);
	if (after > before + 16) FAIL("dentry count grew");

	/* rename onto an existing name frees the target's dentry too */
	for (int i = 0; i < 2000; i++) {
		int fd = open("/tmp/dch/a", O_WRONLY | O_CREAT, 0644);
		close(fd);
		mkdir("/tmp/dch/sub", 0755);
		fd = open("/tmp/dch/sub/b", O_WRONLY | O_CREAT, 0644);
		close(fd);
		if (rename("/tmp/dch/a", "/tmp/dch/sub/b")) FAIL("rename over");
		unlink("/tmp/dch/sub/b");
	}
	rmdir("/tmp/dch/sub");
	if (dentries() > before + 16) FAIL("rename-over leaked");
	printf("churn ok\n");

	/* volatile chains: /proc is no_cache, every hop reuses one dentry */
	struct stat st;
	for (int i = 0; i < 500; i++) {
		if (stat("/proc/self/task", &st)) FAIL("stat chain");
		if (stat("/proc/self/task/..", &st)) FAIL("stat dotdot");
		if (stat("/proc/self/fd/../..", &st)) FAIL("stat dotdot2");
		int fd = open("/proc/self/status", O_RDONLY);
		if (fd < 0) FAIL("open chain");
		char b[32];
		if (read(fd, b, sizeof(b)) <= 0) FAIL("read chain");
		close(fd);
	}
	printf("volatile ok\n");

	/* chroot to a volatile directory has nothing lasting to point at: the root
	 * becomes an empty one (what Linux leaves once the directory's task exits) */
	pid_t c = fork();
	if (c == 0) {
		struct stat s2;
		if (chroot("/proc/self/task")) _exit(1);
		if (stat("/proc", &s2) == 0 || stat("/bin", &s2) == 0) _exit(2);
		/* '..' and '/..' stay at the empty root */
		if (chdir("..") || chdir("/..") || stat("../bin", &s2) == 0 || stat("/../proc", &s2) == 0) _exit(3);
		/* and it lists nothing */
		DIR *d = opendir("/");
		struct dirent *e;
		while (d && (e = readdir(d)))
			if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) _exit(4);
		_exit(0);
	}
	int s;
	waitpid(c, &s, 0);
	if (WEXITSTATUS(s)) { printf("step %d\n", WEXITSTATUS(s)); FAIL("chroot volatile"); }
	printf("chroot ok\nOK\n");
	return 0;
}
