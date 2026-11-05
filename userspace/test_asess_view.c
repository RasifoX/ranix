/* The filesystem view: the root directory reads, nothing writes, nothing above it
 * shows, whatever the route. Files in the view are owned by the session's uid and
 * mode 0666, so the read-only mount is the only thing refusing the writes. */
#define _GNU_SOURCE
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/xattr.h>
#include <time.h>
#include "asess_test.h"

#define JAIL   "/tmp/asess-view"
#define SECRET "/tmp/asess-view-secret"
#define BODY   "unchanged"

static void put(const char *path, const char *body)
{
	int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0666);
	CHECK(fd >= 0 && write(fd, body, strlen(body)) == (ssize_t)strlen(body), "write fixture");
	close(fd);
	CHECK(chmod(path, 0666) == 0 && chown(path, UID, GID) == 0, "fixture owner");
}

static void unreachable(void)
{
	static const char *const routes[] = {
		SECRET,
		"/.." SECRET,
		"/../../../.." SECRET,
		"/proc/self/root" SECRET,
		"/proc/self/root/.." SECRET,
		"/proc/1/root" SECRET,
		"/proc/self/cwd/.." SECRET,
		"/dev/.." SECRET,
		"/tmp/.." SECRET,
		"/proc/self/fd/../../../../.." SECRET,
		"/bin/sh",
		"/../bin/sh",
		"/proc/self/root/bin/sh",
		"/sys",
		"/etc",
	};
	for (unsigned i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
		errno = 0;
		int fd = open(routes[i], O_RDONLY);
		if (fd >= 0 || errno != ENOENT) {
			printf("FAIL reached %s: fd %d errno %d\n", routes[i], fd, errno);
			fails++;
			if (fd >= 0) close(fd);
		}
	}

	DENIED(open("/private/f", O_RDONLY), ENOENT, "file under a mode 0700 directory");

	/* relative walks, chdir, and symlinks aimed outside */
	CHECK(chdir("/data") == 0, "chdir into the view");
	DENIED(open("../../../.." SECRET, O_RDONLY), ENOENT, "relative walk out");
	CHECK(chdir("../../..") == 0, "chdir above root stops at root");
	char cwd[64];
	CHECK(getcwd(cwd, sizeof(cwd)) && !strcmp(cwd, "/"), "getcwd after climbing");
	CHECK(symlink("../.." SECRET, "/tmp/up") == 0, "make a relative symlink");
	DENIED(open("/tmp/up", O_RDONLY), ENOENT, "relative symlink out");
	CHECK(symlink(SECRET, "/tmp/abs") == 0, "make an absolute symlink");
	DENIED(open("/tmp/abs", O_RDONLY), ENOENT, "absolute symlink out");

	int d = open("/data", O_RDONLY | O_DIRECTORY);
	CHECK(d >= 0, "open a directory");
	DENIED(openat(d, "../../.." SECRET, O_RDONLY), ENOENT, "openat out of a dirfd");
	close(d);

	char buf[4096] = "";
	int fd = open("/proc/mounts", O_RDONLY);
	CHECK(fd >= 0, "open /proc/mounts");
	ssize_t n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	buf[n > 0 ? n : 0] = '\0';
	CHECK(n > 0 && !strstr(buf, "asess-view"), "/proc/mounts hides where the root is");
	CHECK(strstr(buf, " /tmp tmpfs") != NULL, "/proc/mounts shows the private /tmp");
}

static void writes_refused(void)
{
	struct stat st;
	char c = 'x';

	DENIED(open("/data/f", O_WRONLY), EROFS, "open for write");
	DENIED(open("/data/f", O_RDWR), EROFS, "open read-write");
	DENIED(open("/data/f", O_RDONLY | O_TRUNC), EROFS, "O_TRUNC");
	DENIED(open("/data/new", O_CREAT | O_WRONLY, 0644), EROFS, "create");
	DENIED(mkdir("/data/d", 0755), EROFS, "mkdir");
	DENIED(rmdir("/data"), EROFS, "rmdir");
	DENIED(unlink("/data/f"), EROFS, "unlink");
	DENIED(rename("/data/f", "/data/g"), EROFS, "rename inside");
	DENIED(rename("/data/f", "/tmp/stolen"), EROFS, "rename out to /tmp");
	DENIED(rename("/tmp/nothing", "/data/g"), EROFS, "rename in from /tmp");
	DENIED(symlink("x", "/data/l"), EROFS, "symlink");
	DENIED(link("/data/f", "/data/h"), EROFS, "hard link inside");
	DENIED(mkfifo("/data/p", 0644), EROFS, "mkfifo");
	DENIED(chmod("/data/f", 0777), EROFS, "chmod");
	DENIED(chown("/data/f", UID, GID), EROFS, "chown");
	DENIED(truncate("/data/f", 0), EROFS, "truncate");
	DENIED(utimensat(AT_FDCWD, "/data/f", NULL, 0), EROFS, "utimensat");
	DENIED(setxattr("/data/f", "user.x", "1", 1, 0), EROFS, "setxattr");
	DENIED(removexattr("/data/f", "user.x"), EROFS, "removexattr");

	/* through a symlink on a writable mount */
	CHECK(symlink("/data/f", "/tmp/link") == 0, "symlink from /tmp");
	DENIED(open("/tmp/link", O_WRONLY), EROFS, "write through a symlink");
	CHECK(symlink("/data", "/tmp/dlink") == 0, "dir symlink from /tmp");
	DENIED(open("/tmp/dlink/f", O_WRONLY), EROFS, "write through a directory symlink");
	DENIED(open("/tmp/dlink/new", O_CREAT | O_WRONLY, 0644), EROFS, "create through a directory symlink");

	/* a link out would hand the inode to the writable mount's flags */
	DENIED(link("/data/f", "/tmp/hard"), EXDEV, "hard link to /tmp");

	/* fd-based, on the only fd the view gives */
	int fd = open("/data/f", O_RDONLY);
	CHECK(fd >= 0, "open read-only");
	DENIED(write(fd, &c, 1), EBADF, "write to a read-only fd");
	DENIED(pwrite(fd, &c, 1, 0), EBADF, "pwrite to a read-only fd");
	DENIED(ftruncate(fd, 0), EINVAL, "ftruncate a read-only fd");
	DENIED(fallocate(fd, 0, 0, 4096), EBADF, "fallocate a read-only fd");
	DENIED(fchmod(fd, 0777), EROFS, "fchmod");
	DENIED(fchown(fd, UID, GID), EROFS, "fchown");
	DENIED(futimens(fd, NULL), EROFS, "futimens");

	void *m = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	CHECK(m == MAP_FAILED && errno == EACCES, "shared writable map of a read-only fd");
	m = mmap(NULL, 4096, PROT_READ, MAP_SHARED, fd, 0);
	CHECK(m != MAP_FAILED, "shared read map");
	if (m != MAP_FAILED) {
		DENIED(mprotect(m, 4096, PROT_READ | PROT_WRITE), EACCES, "mprotect to writable");
		munmap(m, 4096);
	}
	m = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
	CHECK(m != MAP_FAILED, "private writable map is a copy");
	if (m != MAP_FAILED) { *(char *)m = 'x'; munmap(m, 4096); }
	close(fd);

	/* proc is read-only too: its sysctl writers check nothing for some entries */
	DENIED(open("/proc/sys/kernel/core_pattern", O_WRONLY), EROFS, "core_pattern");
	DENIED(open("/proc/sys/kernel/hostname", O_WRONLY), EROFS, "hostname");
	DENIED(open("/proc/self/uid_map", O_WRONLY), EROFS, "uid_map");

	/* a setuid file made in /tmp means nothing */
	CHECK(stat("/data/f", &st) == 0 && st.st_size == sizeof(BODY) - 1, "size intact inside");

	int w = open("/tmp/ok", O_CREAT | O_WRONLY, 0644);
	CHECK(w >= 0 && write(w, "y", 1) == 1, "private /tmp writes");
	close(w);
}

/* The read-only check and the operation used to walk the path separately, so
 * swapping /tmp/sw between a file and a link into the view could slip a write past
 * the check. Two tasks on two CPUs, then the outer checks read the file back. */
static void symlink_swap_race(void)
{
	struct stat ro;
	CHECK(stat("/data/f", &ro) == 0, "stat the target");

	pid_t swapper = fork();
	if (swapper == 0) {
		for (;;) {
			unlink("/tmp/sw");
			symlink("/data/f", "/tmp/sw");
			unlink("/tmp/sw");
			close(open("/tmp/sw", O_CREAT | O_WRONLY, 0666));
		}
	}
	int crossed = 0;
	for (int i = 0; i < 4000; i++) {
		int fd = open("/tmp/sw", O_RDWR);
		if (fd >= 0) {
			struct stat st;
			/* inode numbers repeat across filesystems, and st_dev is one value here */
			if (fstat(fd, &st) == 0 && st.st_ino == ro.st_ino && st.st_mode == ro.st_mode &&
			    st.st_size == ro.st_size) crossed++;
			close(fd);
		}
		truncate("/tmp/sw", 0);
		chmod("/tmp/sw", 0777);
	}
	kill(swapper, 9);
	waitpid(swapper, NULL, 0);
	unlink("/tmp/sw");
	CHECK(!crossed, "no writable fd into the view through a swapped symlink");
}

static void hidden_names(void)
{
	static const char *const dev_absent[] = {
		"/dev/console", "/dev/kmsg", "/dev/tty", "/dev/ptmx", "/dev/pts", "/dev/pts/0",
		"/dev/fb0", "/dev/rtc0", "/dev/input/event0", "/dev/mem", "/dev/sda", "/dev/shm",
	};
	static const char *const proc_absent[] = {
		"/proc/cmdline", "/proc/modules", "/proc/partitions", "/proc/diskstats",
		"/proc/interrupts", "/proc/swaps", "/proc/net", "/proc/asess",
	};
	for (unsigned i = 0; i < sizeof(dev_absent) / sizeof(dev_absent[0]); i++)
		DENIED(open(dev_absent[i], O_RDONLY | O_NONBLOCK), ENOENT, dev_absent[i]);
	for (unsigned i = 0; i < sizeof(proc_absent) / sizeof(proc_absent[0]); i++)
		DENIED(open(proc_absent[i], O_RDONLY), ENOENT, proc_absent[i]);

	char c = 1;
	int fd = open("/dev/zero", O_RDONLY);
	CHECK(fd >= 0 && read(fd, &c, 1) == 1 && c == 0, "/dev/zero");
	close(fd);
	fd = open("/dev/null", O_WRONLY);
	CHECK(fd >= 0 && write(fd, &c, 1) == 1, "/dev/null");
	close(fd);

	DIR *d = opendir("/dev");
	CHECK(d != NULL, "list /dev");
	int n = 0, null_seen = 0;
	for (struct dirent *e; d && (e = readdir(d)); n++) {
		if (!strcmp(e->d_name, "null")) null_seen = 1;
		if (!strcmp(e->d_name, "console") || !strcmp(e->d_name, "ptmx") ||
		    !strcmp(e->d_name, "kmsg")) {
			printf("FAIL /dev lists %s\n", e->d_name);
			fails++;
		}
	}
	if (d) closedir(d);
	CHECK(null_seen && n >= 9, "/dev lists the allowed names");
}

int main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);

	/* the same holes, outside any session */
	put(SECRET, BODY);
	char c = 'x';
	int ro = open(SECRET, O_RDONLY);
	CHECK(ro >= 0, "outside: open");
	DENIED(write(ro, &c, 1), EBADF, "outside: write to a read-only fd");
	DENIED(ftruncate(ro, 0), EINVAL, "outside: ftruncate a read-only fd");
	void *om = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, ro, 0);
	CHECK(om == MAP_FAILED && errno == EACCES, "outside: shared writable map");
	close(ro);

	mkdir(JAIL, 0755);
	mkdir(JAIL "/data", 0755);
	mkdir(JAIL "/tmp", 0755);
	mkdir(JAIL "/proc", 0755);
	mkdir(JAIL "/dev", 0755);
	CHECK(chown(JAIL "/data", UID, GID) == 0 && chmod(JAIL "/data", 0777) == 0, "fixture dir");
	put(JAIL "/data/f", BODY);
	mkdir(JAIL "/private", 0700);
	put(JAIL "/private/f", BODY);   /* readable by all, inside a directory that is not */

	int keep[] = { 1 };
	long sfd = sess_create(JAIL, UID, keep, 1, 0);
	CHECK(sfd >= 0, "create");
	long pid = sess_must_spawn(sfd);
	if (pid == 0) {
		char data[32] = "";
		int fd = open("/data/f", O_RDONLY);
		CHECK(fd >= 0 && read(fd, data, sizeof(data)) == sizeof(BODY) - 1 &&
		      !memcmp(data, BODY, sizeof(BODY) - 1), "the view reads");
		close(fd);
		unreachable();
		writes_refused();
		hidden_names();
		symlink_swap_race();
		_exit(fails ? 1 : 0);
	}
	sess_reap((pid_t)pid);

	char data[32] = "";
	int fd = open(JAIL "/data/f", O_RDONLY);
	ssize_t n = read(fd, data, sizeof(data) - 1);
	close(fd);
	CHECK(n == sizeof(BODY) - 1 && !memcmp(data, BODY, n), "file untouched afterwards");
	struct stat st;
	CHECK(stat(JAIL "/data/f", &st) == 0 && (st.st_mode & 07777) == 0666, "mode untouched");
	CHECK(access(JAIL "/tmp/ok", F_OK) == -1, "private /tmp left no trace in the view");
	CHECK(access("/tmp/ok", F_OK) == -1, "private /tmp left no trace on the host");
	return finish();
}
