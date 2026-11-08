/* Symlinks keep working once the dentry cache is full. Past its limit a name
 * looked up for the first time is not cached but held for the one walk, and a
 * relative symlink met that way must still be followed from its own directory.
 * Found on Casper (2026-11-08/09): after a long boot "/bin/cp -> ../usr/bin/
 * coreutils" gave "not found" while /bin/../usr/bin/coreutils worked.
 *
 *   test_dcachefull DEV MNT [MAX]   DEV ext2 at MNT; MAX the cache limit (131072)
 *   test_dcachefull fill [MAX]      only fill the cache (for a check on a live system)
 *   test_dcachefull unfill          remove what fill made
 *
 * Builds MNT/dc with relative links, then mounts DEV again so none of its names
 * is cached, fills the cache with files under /tmp/dcfill, and then:
 *   dotdot-open   MNT/dc/bin/t -> ../usr/bin/tool reads the tool
 *   same-dir      MNT/dc/usr/bin/t3 -> tool reads it
 *   dotdot-stat   stat through MNT/dc/bin/t2 (-> ../usr/bin/tool) finds a regular file
 *   dotdot-exec   MNT/dc/bin/echo -> ../usr/bin/bb (busybox) runs
 *   chain         MNT/dc/bin/c -> ../lib/c2, MNT/dc/lib/c2 -> ../usr/bin/tool
 * The fill files are removed at the end. Prints PASS/FAIL a case, then OK or
 * FAIL. Runs as root. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>

#define PER_DIR 512

static const char *g_dev, *g_mnt;
static int g_fail;

static void result(const char *name, int ok, const char *why)
{
	printf("%s %s%s%s\n", ok ? "PASS" : "FAIL", name, ok ? "" : ": ", ok ? "" : why);
	if (!ok) g_fail = 1;
}

static const char *M(const char *rel)
{
	static char buf[4][300];
	static int i;
	i = (i + 1) & 3;
	snprintf(buf[i], sizeof buf[i], "%s/dc%s", g_mnt, rel);
	return buf[i];
}

static long dentries(void)
{
	char b[64] = "";
	int fd = open("/proc/sys/fs/dentry-state", O_RDONLY);
	if (fd < 0) return -1;
	ssize_t n = read(fd, b, sizeof b - 1);
	close(fd);
	return n > 0 ? atol(b) : -1;
}

static int reads(const char *path, const char *text)
{
	char b[32] = "";
	int fd = open(path, O_RDONLY);
	if (fd < 0) return 0;
	ssize_t n = read(fd, b, sizeof b - 1);
	close(fd);
	return n == (ssize_t)strlen(text) && !memcmp(b, text, n);
}

static int copy(const char *from, const char *to)
{
	int in = open(from, O_RDONLY), out = open(to, O_WRONLY | O_CREAT | O_TRUNC, 0755);
	char b[65536];
	ssize_t n;
	int ok = in >= 0 && out >= 0;
	while (ok && (n = read(in, b, sizeof b)) > 0) ok = write(out, b, n) == n;
	if (in >= 0) close(in);
	if (out >= 0) close(out);
	return ok;
}

static void setup(void)
{
	char cmd[300];
	snprintf(cmd, sizeof cmd, "rm -rf %s/dc", g_mnt);
	if (system(cmd)) { }
	const char *dirs[] = { "", "/bin", "/lib", "/usr", "/usr/bin" };
	for (unsigned i = 0; i < 5; i++) mkdir(M(dirs[i]), 0755);
	int fd = open(M("/usr/bin/tool"), O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd >= 0) { if (write(fd, "T", 1) != 1) { } close(fd); }
	copy("/bin/busybox", M("/usr/bin/bb"));
	if (symlink("../usr/bin/tool", M("/bin/t")) || symlink("../usr/bin/tool", M("/bin/t2")) ||
	    symlink("tool", M("/usr/bin/t3")) || symlink("../usr/bin/bb", M("/bin/echo")) ||
	    symlink("../lib/c2", M("/bin/c")) || symlink("../usr/bin/tool", M("/lib/c2"))) { }
	sync();
}

/* fill until the cache holds more than max; returns the count reached */
static long fill(long max, long *made)
{
	char p[64];
	mkdir("/tmp/dcfill", 0755);
	*made = 0;
	long have = dentries();
	for (int d = 0; have >= 0 && have <= max + 64 && d < 4096; d++) {
		snprintf(p, sizeof p, "/tmp/dcfill/%d", d);
		mkdir(p, 0755);
		for (int f = 0; f < PER_DIR; f++) {
			snprintf(p, sizeof p, "/tmp/dcfill/%d/%d", d, f);
			int fd = open(p, O_WRONLY | O_CREAT, 0644);
			if (fd < 0) return dentries();
			close(fd);
			(*made)++;
		}
		have = dentries();
	}
	return have;
}

static void unfill(long made)
{
	char p[64];
	if (made < 0) made = 4096L * PER_DIR;
	for (long i = 0; i < made; i++) {
		snprintf(p, sizeof p, "/tmp/dcfill/%ld/%ld", i / PER_DIR, i % PER_DIR);
		unlink(p);
		if (i % PER_DIR == PER_DIR - 1) { snprintf(p, sizeof p, "/tmp/dcfill/%ld", i / PER_DIR); rmdir(p); }
	}
	if (system("rm -rf /tmp/dcfill")) { }
}

static int runs_echo(void)
{
	int pp[2];
	if (pipe(pp)) return 0;
	pid_t c = fork();
	if (c == 0) {
		dup2(pp[1], 1);
		execl(M("/bin/echo"), "echo", "ran", (char *)NULL);
		_exit(127);
	}
	close(pp[1]);
	char b[16] = "";
	ssize_t n = read(pp[0], b, sizeof b - 1);
	close(pp[0]);
	int st = 0;
	waitpid(c, &st, 0);
	return n >= 3 && !strncmp(b, "ran", 3) && WIFEXITED(st) && WEXITSTATUS(st) == 0;
}

int main(int argc, char **argv)
{
	if (argc > 1 && !strcmp(argv[1], "fill")) {
		long made = 0, have = fill(argc > 2 ? atol(argv[2]) : 131072, &made);
		printf("cache %ld after %ld files\n", have, made);
		return 0;
	}
	if (argc > 1 && !strcmp(argv[1], "unfill")) { unfill(-1); printf("cache %ld\n", dentries()); return 0; }
	if (argc < 3) { fprintf(stderr, "usage: test_dcachefull DEV MNT [MAX] | fill [MAX] | unfill\n"); return 2; }
	g_dev = argv[1];
	g_mnt = argv[2];
	long max = argc > 3 ? atol(argv[3]) : 131072;
	setvbuf(stdout, NULL, _IONBF, 0);
	if (dentries() < 0) { printf("FAIL setup: no /proc/sys/fs/dentry-state\nFAIL\n"); return 1; }
	setup();
	/* a fresh mount: nothing below MNT is in the cache */
	if (umount(g_mnt) || mount(g_dev, g_mnt, "ext2", 0, NULL)) {
		printf("FAIL setup: mount again, errno %d\nFAIL\n", errno);
		return 1;
	}
	long made = 0, have = fill(max, &made);
	char why[120];
	snprintf(why, sizeof why, "cache %ld after %ld files, limit %ld", have, made, max);
	if (have <= max) { result("fill", 0, why); unfill(made); printf("FAIL\n"); return 1; }

	errno = 0;
	int ok = reads(M("/bin/t"), "T");
	snprintf(why, sizeof why, "open: errno %d (cache %ld, limit %ld)", errno, have, max);
	result("dotdot-open", ok, why);
	result("same-dir", reads(M("/usr/bin/t3"), "T"), "t3 -> tool did not read");
	struct stat st;
	result("dotdot-stat", stat(M("/bin/t2"), &st) == 0 && S_ISREG(st.st_mode), "stat through ../ link failed");
	result("dotdot-exec", runs_echo(), "exec through ../ link failed");
	result("chain", reads(M("/bin/c"), "T"), "two ../ links in a row did not read");

	unfill(made);
	char cmd[300];
	snprintf(cmd, sizeof cmd, "rm -rf %s/dc", g_mnt);
	if (system(cmd)) { }
	printf("%s\n", g_fail ? "FAIL" : "OK");
	return g_fail;
}
