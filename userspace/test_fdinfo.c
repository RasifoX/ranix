/* /proc/<pid>/fdinfo: one file per open fd with its offset and open flags, in
 * the Linux format; the task's own, closed to other users. And a chroot into
 * /proc/self/fdinfo/ (Chromium's sandbox does it) gives an empty root nothing
 * can be made in. Prints OK, or FAIL with what was wrong. */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define NOBODY 65534
#define FAIL(...) do { printf("FAIL " __VA_ARGS__); putchar('\n'); return 1; } while (0)

static int slurp(const char *path, char *buf, int cap)
{
	int fd = open(path, O_RDONLY);
	if (fd < 0) return -errno;
	int n = (int)read(fd, buf, cap - 1);
	int e = errno;
	close(fd);
	if (n < 0) return -e;
	buf[n] = 0;
	return n;
}

static long field(const char *buf, const char *name, int base)
{
	const char *s = strstr(buf, name);
	return s ? strtol(s + strlen(name), NULL, base) : -1;
}

static int listed(const char *dir, int fd)
{
	DIR *d = opendir(dir);
	if (!d) return 0;
	struct dirent *e;
	int found = 0;
	while ((e = readdir(d))) if (atoi(e->d_name) == fd && e->d_name[0] >= '0') found = 1;
	closedir(d);
	return found;
}

/* own fds: offset, flags, O_CLOEXEC, the listing */
static int own(void)
{
	char p[64], b[256];
	int fd = open("/tmp/L_fdinfo", O_RDWR | O_CREAT | O_TRUNC, 0600);
	if (fd < 0) FAIL("create %d", errno);
	if (write(fd, "0123456789", 10) != 10) FAIL("write");
	lseek(fd, 7, SEEK_SET);
	snprintf(p, sizeof p, "/proc/self/fdinfo/%d", fd);
	if (slurp(p, b, sizeof b) <= 0) FAIL("read %s %d", p, errno);
	if (strncmp(b, "pos:\t7\nflags:\t0", 15)) FAIL("format '%s'", b);
	if ((field(b, "flags:\t", 8) & 3) != O_RDWR) FAIL("flags not O_RDWR '%s'", b);
	if (!strstr(b, "\nmnt_id:\t")) FAIL("no mnt_id '%s'", b);
	int cfd = open("/tmp/L_fdinfo", O_RDONLY | O_CLOEXEC);
	snprintf(p, sizeof p, "/proc/self/fdinfo/%d", cfd);
	if (slurp(p, b, sizeof b) <= 0) FAIL("read cloexec %d", errno);
	if (!(field(b, "flags:\t", 8) & O_CLOEXEC)) FAIL("O_CLOEXEC missing '%s'", b);
	if (!listed("/proc/self/fdinfo", fd) || !listed("/proc/self/fdinfo", cfd)) FAIL("not listed");
	close(cfd);
	snprintf(p, sizeof p, "/proc/self/fdinfo/%d", cfd);
	if (slurp(p, b, sizeof b) >= 0) FAIL("closed fd still has fdinfo");
	struct stat st;
	if (stat("/proc/self/fdinfo", &st) || !S_ISDIR(st.st_mode)) FAIL("fdinfo not a directory");
	close(fd);
	unlink("/tmp/L_fdinfo");
	return 0;
}

/* nobody may not read or list root's */
static int others(void)
{
	int go[2];
	if (pipe(go)) FAIL("pipe");
	pid_t t = fork();
	if (t == 0) { char x; close(go[1]); if (read(go[0], &x, 1) < 0) { } _exit(0); }
	close(go[0]);
	char p[64], b[256];
	snprintf(p, sizeof p, "/proc/%d/fdinfo/0", (int)t);
	int root_ok = slurp(p, b, sizeof b) > 0;
	pid_t o = fork();
	if (o == 0) {
		if (setgid(NOBODY) || setuid(NOBODY)) _exit(2);
		char d[64];
		snprintf(d, sizeof d, "/proc/%d/fdinfo", (int)t);
		if (slurp(p, b, sizeof b) >= 0) _exit(3);
		if (listed(d, 0)) _exit(4);
		_exit(0);
	}
	int st = 0;
	waitpid(o, &st, 0);
	close(go[1]);
	waitpid(t, NULL, 0);
	if (!root_ok) FAIL("root could not read a child's fdinfo");
	if (!WIFEXITED(st) || WEXITSTATUS(st)) FAIL("nobody read root's fdinfo (rc %d)", WEXITSTATUS(st));
	return 0;
}

/* chroot into /proc/self/fdinfo/ of a child: an empty root */
static int chroot_fdinfo(void)
{
	pid_t c = fork();
	if (c == 0) {
		if (chroot("/proc/self/fdinfo/")) _exit(10);
		if (chdir("/")) _exit(11);
		struct stat st;
		if (stat("/proc", &st) == 0 || errno != ENOENT) _exit(12);
		if (stat("/bin", &st) == 0) _exit(13);
		if (mkdir("/x", 0755) == 0) _exit(14);
		if (open("/f", O_WRONLY | O_CREAT, 0600) >= 0) _exit(15);
		if (stat("/..", &st) || !S_ISDIR(st.st_mode)) _exit(16);
		if (stat("/../../bin", &st) == 0) _exit(17);
		DIR *d = opendir("/");
		struct dirent *e;
		int names = 0;
		while (d && (e = readdir(d))) if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) names++;
		if (names) _exit(18);
		_exit(0);
	}
	int st = 0;
	waitpid(c, &st, 0);
	if (!WIFEXITED(st) || WEXITSTATUS(st)) FAIL("chroot into fdinfo: step %d", WEXITSTATUS(st));
	return 0;
}

int main(void)
{
	if (getuid() != 0) { puts("FAIL run as root"); return 0; }
	if (own() || others() || chroot_fdinfo()) return 0;
	puts("OK");
	return 0;
}
