/* One directory of the host bound writable into a session: writes land on the
 * host at once, and nothing else of the host becomes writable or visible. Both
 * shapes: a directory the view has anyway, and one under the private /tmp, which
 * comes from another tree and has a real parent the session must not reach. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include "asess_test.h"

#define SAME "/var/proj"      /* inside root: the view reaches it by itself */
#define CROSS "/tmp/bp/proj"  /* under the private /tmp: a real mount */

#define MIB (1024L * 1024)

static long bind_create(const char *root, const char *dir, const int *keep, int nkeep,
                        unsigned long long mem)
{
	struct asess_attr a;
	memset(&a, 0, sizeof(a));
	a.size = sizeof(a);
	a.uid = UID;
	a.gid = GID;
	a.root = (unsigned long long)(unsigned long)root;
	a.rw_dir = (unsigned long long)(unsigned long)dir;
	a.keep_fds = (unsigned long long)(unsigned long)keep;
	a.nkeep = (unsigned)nkeep;
	a.mem_max = mem;
	return asess(ASESS_CREATE, -1, &a, sizeof(a));
}

static int put(const char *p, const char *s)
{
	int fd = open(p, O_CREAT | O_WRONLY | O_TRUNC, 0644);
	if (fd < 0) return -1;
	ssize_t n = write(fd, s, strlen(s));
	close(fd);
	return n == (ssize_t)strlen(s) ? 0 : -1;
}

static int got(const char *p, const char *want)
{
	char b[32];
	int fd = open(p, O_RDONLY);
	if (fd < 0) return 0;
	ssize_t n = read(fd, b, sizeof(b) - 1);
	close(fd);
	if (n < 0) return 0;
	b[n] = 0;
	return strcmp(b, want) == 0;
}

/* what the session may do in its directory */
static void inside(const char *dir)
{
	char a[80], b[80];

	snprintf(a, sizeof(a), "%s/made", dir);
	CHECK(put(a, "session") == 0, "write in the bound directory");
	CHECK(got(a, "session"), "read back");
	snprintf(b, sizeof(b), "%s/renamed", dir);
	CHECK(rename(a, b) == 0, "rename within");
	snprintf(a, sizeof(a), "%s/link", dir);
	CHECK(link(b, a) == 0 && unlink(a) == 0, "hard link within");
	CHECK(symlink("renamed", a) == 0 && got(a, "session") && unlink(a) == 0, "symlink within");
	snprintf(a, sizeof(a), "%s/sub", dir);
	CHECK(mkdir(a, 0755) == 0, "mkdir");
	snprintf(a, sizeof(a), "%s/sub/deep", dir);
	CHECK(put(a, "x") == 0 && unlink(a) == 0, "write below");
	snprintf(a, sizeof(a), "%s/sub", dir);
	CHECK(rmdir(a) == 0, "rmdir");
	snprintf(a, sizeof(a), "%s/run.sh", dir);
	CHECK(put(a, "#!/bin/sh\nexit 7\n") == 0 && chmod(a, 0755) == 0, "script");
	pid_t p = fork();
	if (p == 0) { execl(a, a, (char *)NULL); _exit(9); }
	int s = 0;
	waitpid(p, &s, 0);
	CHECK(WIFEXITED(s) && WEXITSTATUS(s) == 7, "exec from the bound directory");
	CHECK(unlink(a) == 0, "unlink");

	/* ways out of it */
	snprintf(a, sizeof(a), "%s/made2", dir);
	CHECK(put(a, "y") == 0, "write");
	DENIED(rename(a, "/etc/out"), EROFS, "rename onto the read-only root");
	DENIED(rename(a, "/tmp/out"), EXDEV, "rename to /tmp");
	DENIED(link(a, "/etc/out"), EROFS, "link onto the read-only root");
	DENIED(link(a, "/tmp/out"), EXDEV, "link to /tmp");
	CHECK(unlink(a) == 0, "unlink");

	snprintf(a, sizeof(a), "%s/esc", dir);
	CHECK(symlink("/etc/bind-probe", a) == 0, "symlink out");
	DENIED(open(a, O_WRONLY | O_CREAT, 0644), EROFS, "write through a symlink out");
	CHECK(unlink(a) == 0, "unlink");

	DENIED(mknod("/tmp/null", S_IFCHR | 0666, 0x103), EPERM, "device node");
	snprintf(a, sizeof(a), "%s/null", dir);
	DENIED(mknod(a, S_IFCHR | 0666, 0x103), EPERM, "device node in the bound directory");

	DENIED(open("/etc/bind-probe", O_CREAT | O_WRONLY, 0644), EROFS, "the root stays read-only");
	DENIED(mkdir("/bin/probe", 0755), EROFS, "the root stays read-only");
	DENIED(unlink("/bin/sh"), EROFS, "the root stays read-only");
	DENIED(mount("t", "/tmp", "tmpfs", 0, NULL), EPERM, "mount");
}

/* .. from the bind root lands in the view, not in the tree it came from */
static void cross_parent(void)
{
	struct stat st;
	DENIED(stat(CROSS "/../host-only", &st), ENOENT, "the real parent of the bind");
	DENIED(stat(CROSS "/../../host-marker", &st), ENOENT, "the real grandparent");
	char b[64];
	CHECK(chdir(CROSS) == 0, "chdir");
	DENIED(open("../host-only", O_RDONLY), ENOENT, "relative ..");
	CHECK(chdir("..") == 0 && getcwd(b, sizeof(b)) && strcmp(b, "/tmp/bp") == 0, "chdir ..");
	CHECK(put("/tmp/bp/mine", "z") == 0, "the view's own /tmp");
}

static void run(const char *dir, void (*extra)(void))
{
	static const int keep[] = { 1 };
	long sfd = bind_create("/", dir, keep, 1, 0);
	CHECK(sfd >= 0, "create");
	if (sfd < 0) return;
	long pid = sess_must_spawn(sfd);
	if (pid == 0) {
		inside(dir);
		if (extra) extra();
		_exit(fails ? 1 : 0);
	}
	char made[80];
	sess_reap((pid_t)pid);
	snprintf(made, sizeof(made), "%s/renamed", dir);
	CHECK(got(made, "session"), "the host sees the write");
	struct stat st;
	CHECK(stat(made, &st) == 0 && st.st_uid == UID, "owned by the session's user");
	CHECK(unlink(made) == 0, "host cleans up");
	close((int)sfd);
}

static void refused(void)
{
	errno = 0;
	CHECK(bind_create("/", "/no/such", NULL, 0, 0) < 0 && errno == ENOENT, "missing directory");
	CHECK(bind_create("/", "/bin/sh", NULL, 0, 0) < 0 && errno == ENOTDIR, "not a directory");
	CHECK(bind_create("/", "relative", NULL, 0, 0) < 0 && errno == EINVAL, "relative");
	CHECK(bind_create("/", "/tmp", NULL, 0, 0) < 0 && errno == EBUSY, "the view's own mount");
	CHECK(bind_create("/", "/var/link/x", NULL, 0, 0) < 0, "symlink in the path");
	/* a root that does not have the path */
	CHECK(bind_create("/var/jail", "/etc", NULL, 0, 0) < 0 && errno == ENOENT, "path not in the root");
}

/* the host's end: the bound directory cannot go while a session holds it */
static void busy(void)
{
	static const int keep[] = { 1 };
	int p[2];
	CHECK(pipe(p) == 0, "pipe");
	int k2[] = { 1, p[0] };
	CHECK(mkdir("/var/b1", 0777) == 0 && chmod("/var/b1", 0777) == 0, "mkdir");
	long sfd = bind_create("/", "/var/b1", k2, 2, 0);
	CHECK(sfd >= 0, "create");
	long pid = sess_must_spawn(sfd);
	if (pid == 0) {
		char c;
		read(p[0], &c, 1);
		CHECK(put("/var/b1/f", "still") == 0, "write after the host tried");
		_exit(fails ? 1 : 0);
	}
	close(p[0]);
	DENIED(rmdir("/var/b1"), EBUSY, "rmdir under a session");
	CHECK(write(p[1], "x", 1) == 1, "go");
	close(p[1]);
	sess_reap((pid_t)pid);
	close((int)sfd);
	CHECK(unlink("/var/b1/f") == 0, "cleanup");
	CHECK(rmdir("/var/b1") == 0, "rmdir after the session");

	/* a tmpfs holding the directory stays mounted as long, and frees afterwards */
	CHECK(mkdir("/var/t1", 0755) == 0, "mkdir");
	CHECK(mount("t", "/var/t1", "tmpfs", 0, "size=32M") == 0, "mount");
	CHECK(mkdir("/var/t1/d", 0777) == 0, "mkdir");
	sfd = bind_create("/var/t1", "/var/t1/d", keep, 1, 0);
	/* that root has no /proc, /tmp or /dev, which only narrows it */
	CHECK(sfd >= 0, "create on a tmpfs");
	DENIED(umount("/var/t1"), EBUSY, "umount under a session");
	close((int)sfd);
	CHECK(umount("/var/t1") == 0, "umount after the session");
	CHECK(rmdir("/var/t1") == 0, "rmdir");
}

/* data written into a RAM filesystem on the host counts against the session,
 * after it too, until the host deletes the file or takes it over */
static void budget(void)
{
	static const int keep[] = { 1 };
	static char pg[4096];
	CHECK(mkdir("/var/m1", 0777) == 0 && chmod("/var/m1", 0777) == 0, "mkdir");
	long sfd = bind_create("/", "/var/m1", keep, 1, 16 * MIB);
	CHECK(sfd >= 0, "create");
	long pid = sess_must_spawn(sfd);
	if (pid == 0) {
		int fd = open("/var/m1/big", O_CREAT | O_WRONLY, 0644);
		long n = 0;
		for (;;) {
			ssize_t w = write(fd, pg, sizeof(pg));
			if (w != (ssize_t)sizeof(pg)) break;
			n += w;
		}
		CHECK(errno == ENOSPC, "a RAM directory on the host stops at the budget");
		CHECK(n > 0 && n <= 16 * MIB, "written within the budget");
		close(fd);
		_exit(fails ? 1 : 0);
	}
	sess_reap((pid_t)pid);
	struct asess_info in;
	CHECK(asess(ASESS_INFO, (int)sfd, &in, sizeof(in)) == 0, "info");
	CHECK(in.mem_cur >= 8 * MIB, "the file stays charged");
	close((int)sfd);
	CHECK(unlink("/var/m1/big") == 0 && rmdir("/var/m1") == 0, "cleanup");
}

int main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	mkdir("/var", 0755);
	CHECK(mkdir(SAME, 0777) == 0 || errno == EEXIST, "mkdir");
	chmod(SAME, 0777);
	mkdir("/var/jail", 0755);
	symlink("/var", "/var/link");
	mkdir("/tmp/bp", 0755);
	CHECK(mkdir(CROSS, 0777) == 0 || errno == EEXIST, "mkdir");
	chmod(CROSS, 0777);
	CHECK(put("/tmp/bp/host-only", "h") == 0, "marker");
	CHECK(put("/tmp/host-marker", "h") == 0, "marker");

	refused();
	run(SAME, NULL);
	run(CROSS, cross_parent);
	busy();
	budget();
	return finish();
}
