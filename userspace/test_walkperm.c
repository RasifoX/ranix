/* Search permission: a directory without x for the caller stops every path that
 * goes through it, for every kind of syscall, with EACCES. Run as root, the test
 * builds the tree and drops to an ordinary user. */
#define _GNU_SOURCE
#include <dirent.h>
#include <fcntl.h>
#include <grp.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/un.h>
#include <sys/time.h>
#include <sys/xattr.h>
#include "asess_test.h"

#define T "/tmp/wp"
#define SECRET T "/secret"

extern char **environ;

static void mkfile(const char *p, int mode)
{
	int fd = open(p, O_CREAT | O_WRONLY, 0644);
	CHECK(fd >= 0 && write(fd, "hi", 2) == 2, "create");
	close(fd);
	CHECK(chmod(p, (mode_t)mode) == 0, "chmod");
}

static void denied_everywhere(void)
{
	struct stat st;
	struct statfs sf;
	char b[64];
	char *argv[] = { SECRET "/x", NULL };

	DENIED(open(SECRET "/f", O_RDONLY), EACCES, "open");
	DENIED(open(SECRET "/new", O_CREAT | O_WRONLY, 0644), EACCES, "create");
	DENIED(open(SECRET "/sub/g", O_RDONLY), EACCES, "open below");
	DENIED(open(SECRET, O_RDONLY), EACCES, "open the directory itself");
	DENIED(stat(SECRET "/f", &st), EACCES, "stat");
	DENIED(lstat(SECRET "/f", &st), EACCES, "lstat, last directory of the path");
	DENIED(access(SECRET "/f", F_OK), EACCES, "access");
	DENIED(chdir(SECRET "/sub"), EACCES, "chdir");
	DENIED(chdir(SECRET), EACCES, "chdir into the directory itself");
	DENIED(readlink(SECRET "/l", b, sizeof(b)), EACCES, "readlink");
	DENIED(execve(SECRET "/x", argv, environ), EACCES, "execve");
	DENIED(unlink(SECRET "/f"), EACCES, "unlink");
	DENIED(rmdir(SECRET "/sub"), EACCES, "rmdir");
	DENIED(mkdir(SECRET "/d", 0755), EACCES, "mkdir");
	DENIED(mkfifo(SECRET "/p", 0644), EACCES, "mkfifo");
	DENIED(symlink("/x", SECRET "/s"), EACCES, "symlink");
	DENIED(link(T "/open/mine", SECRET "/h"), EACCES, "link into");
	DENIED(link(SECRET "/f", T "/open/h"), EACCES, "link out of");
	DENIED(rename(SECRET "/f", T "/open/r"), EACCES, "rename out of");
	DENIED(rename(T "/open/mine", SECRET "/r"), EACCES, "rename into");
	DENIED(truncate(SECRET "/f", 0), EACCES, "truncate");
	DENIED(chmod(SECRET "/f", 0777), EACCES, "chmod");
	DENIED(lchown(SECRET "/f", 1000, 1000), EACCES, "lchown");
	DENIED(utimes(SECRET "/f", NULL), EACCES, "utimes");
	DENIED(statfs(SECRET "/f", &sf), EACCES, "statfs");
	DENIED(getxattr(SECRET "/f", "user.a", b, sizeof(b)), EACCES, "getxattr");
	DENIED(setxattr(SECRET "/f", "user.a", "b", 1, 0), EACCES, "setxattr");
	DENIED(open(T "/open/in", O_RDONLY), EACCES, "a symlink into it");
	DENIED(open(T "/open/../secret/f", O_RDONLY), EACCES, "through ..");
	DENIED(chdir(T "/open/in-dir"), EACCES, "chdir through a symlink");
}

static void what_stays_open(void)
{
	char b[64];
	struct stat st;
	int fd;

	CHECK(open(T "/open/mine", O_RDONLY) >= 0, "own file");
	CHECK(stat("/proc/self/status", &st) == 0, "/proc/self");
	fd = open("/proc/self/stat", O_RDONLY);
	CHECK(fd >= 0 && read(fd, b, sizeof(b)) > 0, "read /proc/self/stat");
	close(fd);
	CHECK(open("/proc/1/stat", O_RDONLY) >= 0, "/proc/1");
	CHECK(readlink("/proc/self/fd/0", b, sizeof(b)) > 0 || errno == ENOENT, "/proc/self/fd");
	CHECK(open("/dev/null", O_WRONLY) >= 0, "/dev/null");
	CHECK(open("/etc", O_RDONLY) >= 0, "/etc");
	CHECK(chdir(T "/open") == 0, "chdir");
	CHECK(open("mine", O_RDONLY) >= 0, "relative");
	CHECK(open("../open/mine", O_RDONLY) >= 0, "relative through ..");

	/* execute only: names work, listing does not */
	CHECK(open(T "/xonly/f", O_RDONLY) >= 0, "file in a 0711 directory");
	DENIED(open(T "/xonly", O_RDONLY), EACCES, "list a 0711 directory");

	/* read only: listing works, names do not */
	DIR *d = opendir(T "/ronly");
	CHECK(d != NULL, "open a 0744 directory");
	if (d) {
		int n = 0;
		while (readdir(d)) n++;
		CHECK(n >= 3, "list a 0744 directory");
		closedir(d);
	}
	DENIED(open(T "/ronly/f", O_RDONLY), EACCES, "file in a 0744 directory");
	DENIED(lstat(T "/ronly/f", &st), EACCES, "lstat in a 0744 directory");

	/* group bits, primary and supplementary */
	CHECK(open(T "/grp/f", O_RDONLY) >= 0, "file through a group directory");
}

static void no_group(void)
{
	DENIED(open(T "/grp/f", O_RDONLY), EACCES, "not in the group");
}

/* permission on directories it may not change, with x and without w */
static void read_only_parent(void)
{
	DENIED(rename(T "/shut/f", T "/open/r"), EACCES, "rename out of a shut directory");
	DENIED(rename(T "/open/mine", T "/shut/r"), EACCES, "rename into a shut directory");
	DENIED(symlink("/x", T "/shut/s"), EACCES, "symlink in a shut directory");
	DENIED(link(T "/open/mine", T "/shut/h"), EACCES, "link in a shut directory");
	DENIED(truncate(T "/shut/f", 0), EACCES, "truncate of another's file");
}

#define SOCK SECRET "/s"
#define OPEN_SOCK T "/xonly/s"

static int unix_connect(const char *path)
{
	struct sockaddr_un a = { .sun_family = AF_UNIX };
	strcpy(a.sun_path, path);
	int s = socket(AF_UNIX, SOCK_STREAM, 0);
	if (s < 0) return -1;
	int r = connect(s, (struct sockaddr *)&a, sizeof(a));
	int e = errno;
	close(s);
	errno = e;
	return r;
}

static int unix_listen(const char *path)
{
	struct sockaddr_un a = { .sun_family = AF_UNIX };
	strcpy(a.sun_path, path);
	int s = socket(AF_UNIX, SOCK_STREAM, 0);
	CHECK(s >= 0 && bind(s, (struct sockaddr *)&a, sizeof(a)) == 0 && listen(s, 1) == 0, "listen");
	return s;
}

static void unix_sockets(void)
{
	DENIED(unix_connect(SOCK), EACCES, "connect through a shut directory");
	struct sockaddr_un a = { .sun_family = AF_UNIX };
	strcpy(a.sun_path, SECRET "/mine");
	int s = socket(AF_UNIX, SOCK_STREAM, 0);
	DENIED(bind(s, (struct sockaddr *)&a, sizeof(a)), EACCES, "bind through a shut directory");
	close(s);
	CHECK(unix_connect(OPEN_SOCK) == 0, "connect through an open directory");
}

/* What was reached before dropping privileges stays reached, by cwd and dirfd, and
 * only that: each step away from it is asked again, and the anchor's own x too. */
#define HELD   T "/held"          /* 0700 */
#define INSIDE HELD "/in"         /* 0755 */
#define BESIDE HELD "/beside"     /* 0755 */

static void kept_after_drop(int dfd)
{
	struct stat st;
	CHECK(open("f", O_RDONLY) >= 0, "cwd-relative after the drop");
	CHECK(stat("f", &st) == 0, "stat relative to cwd");
	CHECK(openat(dfd, "g", O_RDONLY) >= 0, "dirfd-relative after the drop");
	CHECK(fstatat(dfd, "g", &st, 0) == 0, "fstatat from a dirfd");
	DENIED(open("../beside/g", O_RDONLY), EACCES, "out of cwd, through the shut parent");
	DENIED(openat(dfd, "../in/f", O_RDONLY), EACCES, "out of a dirfd, through the shut parent");
	DENIED(open(BESIDE "/g", O_RDONLY), EACCES, "a sibling by absolute path");
	DENIED(open(HELD "/h", O_RDONLY), EACCES, "the shut parent's own file");

	CHECK(chdir(SECRET "/sub") != 0, "cannot walk in");
}

/* cwd in a directory without x for the task: its own search bit still counts */
static void cwd_itself_shut(void)
{
	DENIED(open("f", O_RDONLY), EACCES, "cwd without x");
}

int main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	if (getuid() != 0) { puts("SKIP needs root"); return 0; }

	CHECK(mkdir(T, 0755) == 0 || errno == EEXIST, "mkdir");
	CHECK(mkdir(SECRET, 0700) == 0, "secret");
	CHECK(mkdir(SECRET "/sub", 0755) == 0, "sub");
	mkfile(SECRET "/f", 0644);
	mkfile(SECRET "/sub/g", 0644);
	mkfile(SECRET "/x", 0755);
	CHECK(symlink("/etc", SECRET "/l") == 0, "symlink");
	CHECK(mkdir(T "/open", 0777) == 0, "open");
	mkfile(T "/open/mine", 0644);
	CHECK(chown(T "/open/mine", UID, GID) == 0, "chown");
	CHECK(symlink(SECRET "/f", T "/open/in") == 0, "symlink in");
	CHECK(symlink(SECRET "/sub", T "/open/in-dir") == 0, "symlink in");
	CHECK(mkdir(T "/xonly", 0711) == 0, "xonly");
	mkfile(T "/xonly/f", 0644);
	CHECK(mkdir(T "/ronly", 0744) == 0, "ronly");
	mkfile(T "/ronly/f", 0644);
	CHECK(mkdir(T "/grp", 0750) == 0, "grp");
	mkfile(T "/grp/f", 0644);
	CHECK(chown(T "/grp", 0, 2000) == 0, "chown grp");
	CHECK(mkdir(T "/shut", 0755) == 0, "shut");
	mkfile(T "/shut/f", 0644);

	int ls = unix_listen(SOCK), ols = unix_listen(OPEN_SOCK);
	/* connect needs write on the socket file: open this one to all, as a
	 * server meant for everyone would (umask left it 0755) */
	CHECK(chmod(OPEN_SOCK, 0777) == 0, "chmod open socket");
	CHECK(mkdir(HELD, 0700) == 0 && mkdir(INSIDE, 0755) == 0 && mkdir(BESIDE, 0755) == 0, "held");
	mkfile(INSIDE "/f", 0644);
	mkfile(BESIDE "/g", 0644);
	mkfile(HELD "/h", 0644);
	int dfd = open(BESIDE, O_RDONLY | O_DIRECTORY);
	CHECK(dfd >= 0, "open the dirfd");

	pid_t c = fork();
	if (c == 0) {
		CHECK(chdir(INSIDE) == 0, "chdir");
		if (setgroups(0, NULL) || setgid(GID) || setuid(UID)) _exit(1);
		kept_after_drop(dfd);
		_exit(fails ? 2 : 0);
	}
	sess_reap(c);

	c = fork();
	if (c == 0) {
		CHECK(chdir(SECRET) == 0, "chdir");
		if (setgroups(0, NULL) || setgid(GID) || setuid(UID)) _exit(1);
		cwd_itself_shut();
		_exit(fails ? 2 : 0);
	}
	sess_reap(c);

	c = fork();
	if (c == 0) {
		if (setgroups(0, NULL) || setgid(GID) || setuid(UID)) _exit(1);
		unix_sockets();
		_exit(fails ? 2 : 0);
	}
	sess_reap(c);
	CHECK(unix_connect(SOCK) == 0, "root connects through 0700");
	close(ls);
	close(ols);

	c = fork();
	if (c == 0) {
		gid_t g = 2000;
		if (setgroups(1, &g) || setgid(GID) || setuid(UID)) _exit(1);
		denied_everywhere();
		what_stays_open();
		read_only_parent();
		_exit(fails ? 2 : 0);
	}
	sess_reap(c);

	c = fork();
	if (c == 0) {
		if (setgroups(0, NULL) || setgid(GID) || setuid(UID)) _exit(1);
		no_group();
		_exit(fails ? 2 : 0);
	}
	sess_reap(c);

	/* root is not stopped, and the tree is untouched */
	CHECK(open(SECRET "/f", O_RDONLY) >= 0, "root reads through 0700");
	struct stat st;
	CHECK(stat(SECRET "/f", &st) == 0 && st.st_size == 2, "file intact");
	return finish();
}
