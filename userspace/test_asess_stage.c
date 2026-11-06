/* A host directory bound at another directory's place in a session (rw_src): the
 * session reads and writes the copy, the place it covers stays as it was, and no
 * way out of the bind reaches the covered directory or the host's copy by path. */
#define _GNU_SOURCE
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include "asess_test.h"

#define BASE  "/var/sbase"     /* the place, under the session's root */
#define STAGE "/tmp/sstage"    /* the host's copy, hidden behind the private /tmp */

static long stage_create(const char *dir, const char *src, const int *keep, int nkeep)
{
	struct asess_attr a;
	memset(&a, 0, sizeof(a));
	a.size = sizeof(a);
	a.uid = UID;
	a.gid = GID;
	a.root = (unsigned long long)(unsigned long)"/";
	a.rw_dir = (unsigned long long)(unsigned long)dir;
	a.rw_src = (unsigned long long)(unsigned long)src;
	a.keep_fds = (unsigned long long)(unsigned long)keep;
	a.nkeep = (unsigned)nkeep;
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
	char b[64];
	int fd = open(p, O_RDONLY);
	if (fd < 0) return 0;
	ssize_t n = read(fd, b, sizeof(b) - 1);
	close(fd);
	if (n < 0) return 0;
	b[n] = 0;
	return strcmp(b, want) == 0;
}

static int entries(const char *dir)
{
	DIR *d = opendir(dir);
	if (!d) return -1;
	int n = 0;
	struct dirent *de;
	while ((de = readdir(d)))
		if (strcmp(de->d_name, ".") && strcmp(de->d_name, "..")) n++;
	closedir(d);
	return n;
}

static void inside(void)
{
	struct stat st;
	CHECK(got(BASE "/orig", "copy"), "the session reads the copy, not the place");
	CHECK(put(BASE "/orig", "agent") == 0, "write");
	CHECK(put(BASE "/new", "n") == 0, "create");
	CHECK(mkdir(BASE "/d", 0755) == 0 && put(BASE "/d/deep", "x") == 0, "nested");
	CHECK(rename(BASE "/new", BASE "/d/new") == 0, "rename");
	CHECK(symlink("orig", BASE "/ln") == 0 && got(BASE "/ln", "agent"), "symlink within");
	CHECK(link(BASE "/orig", BASE "/hard") == 0, "hard link within");
	CHECK(truncate(BASE "/hard", 2) == 0 && got(BASE "/orig", "ag"), "truncate");

	int fd = open(BASE "/mm", O_CREAT | O_RDWR, 0644);
	CHECK(fd >= 0 && ftruncate(fd, 4096) == 0, "mmap file");
	char *m = fd >= 0 ? mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0) : MAP_FAILED;
	CHECK(m != MAP_FAILED, "mmap shared");
	if (m != MAP_FAILED) { memcpy(m, "mapped", 6); msync(m, 4096, MS_SYNC); munmap(m, 4096); }
	if (fd >= 0) close(fd);

	/* ways out */
	DENIED(rename(BASE "/orig", "/var/out"), EROFS, "rename beside the place");
	DENIED(link(BASE "/orig", "/var/out"), EROFS, "link beside the place");
	DENIED(rename(BASE "/orig", "/tmp/out"), EXDEV, "rename to /tmp");
	CHECK(symlink("/var/shost", BASE "/esc") == 0, "symlink out");
	DENIED(open(BASE "/esc", O_WRONLY | O_CREAT, 0644), EROFS, "write through a symlink out");
	DENIED(open(BASE "/../shost", O_WRONLY | O_CREAT, 0644), EROFS, ".. lands in the read-only view");
	CHECK(stat(BASE "/../sbase/orig", &st) == 0 && st.st_size == 2, ".. and back is the copy");
	DENIED(stat(STAGE "/orig", &st), ENOENT, "the host's copy has no path inside");
	DENIED(mknod(BASE "/null", S_IFCHR | 0666, 0x103), EPERM, "device node");
	CHECK(chdir(BASE) == 0 && chdir("..") == 0, "chdir ..");
	DENIED(open("sbase-x", O_WRONLY | O_CREAT, 0644), EROFS, "relative write beside");
	DENIED(asess(ASESS_INFO, 3, NULL, 0), EPERM, "no sfd inside");
}

static void run(void)
{
	static const int keep[] = { 1 };
	long sfd = stage_create(BASE, STAGE, keep, 1);
	CHECK(sfd >= 0, "create");
	if (sfd < 0) return;
	long pid = sess_must_spawn(sfd);
	if (pid == 0) { inside(); _exit(fails ? 1 : 0); }
	sess_reap((pid_t)pid);
	close((int)sfd);

	CHECK(got(BASE "/orig", "base"), "the place is untouched");
	CHECK(entries(BASE) == 1, "nothing new at the place");
	CHECK(got(STAGE "/orig", "ag"), "the copy has the write");
	CHECK(got(STAGE "/d/new", "n") && got(STAGE "/d/deep", "x"), "the copy has the tree");
	char b[8] = { 0 };
	int fd = open(STAGE "/mm", O_RDONLY);
	CHECK(fd >= 0 && read(fd, b, 6) == 6 && !memcmp(b, "mapped", 6), "a shared mapping lands in the copy");
	if (fd >= 0) close(fd);
	struct stat st;
	DENIED(stat("/var/shost", &st), ENOENT, "nothing beside the place");
}

/* a session killed in the middle of writing leaves the place as it was */
static void killed(void)
{
	int p[2];
	CHECK(pipe(p) == 0, "pipe");
	int keep[] = { 1, p[1] };
	CHECK(put(STAGE "/orig", "copy") == 0, "reset");
	long sfd = stage_create(BASE, STAGE, keep, 2);
	CHECK(sfd >= 0, "create");
	if (sfd < 0) return;
	long pid = sess_must_spawn(sfd);
	if (pid == 0) {
		static char blk[4096];
		int fd = open(BASE "/big", O_CREAT | O_WRONLY, 0644);
		for (int i = 0; ; i++) {
			if (write(fd, blk, sizeof(blk)) != (ssize_t)sizeof(blk)) break;
			if (i == 16) (void)!write(p[1], "g", 1);
			if (i > 4096) lseek(fd, 0, SEEK_SET);
		}
		_exit(0);
	}
	close(p[1]);
	char c;
	CHECK(read(p[0], &c, 1) == 1, "writing");
	close(p[0]);
	close((int)sfd);   /* the last sfd: the session dies */
	int st = 0;
	CHECK(waitpid((pid_t)pid, &st, 0) == pid && WIFSIGNALED(st), "killed");
	CHECK(got(BASE "/orig", "base") && entries(BASE) == 1, "the place is untouched after a kill");
	struct stat s;
	CHECK(stat(STAGE "/big", &s) == 0 && s.st_size > 0, "the cut write is in the copy only");
}

static void refused(void)
{
	errno = 0;
	CHECK(stage_create(NULL, STAGE, NULL, 0) < 0 && errno == EINVAL, "rw_src without rw_dir");
	CHECK(stage_create(BASE, "/no/such", NULL, 0) < 0 && errno == ENOENT, "missing copy");
	CHECK(stage_create(BASE, "/bin/sh", NULL, 0) < 0 && errno == ENOTDIR, "copy not a directory");
	CHECK(stage_create(BASE, "rel", NULL, 0) < 0 && errno == EINVAL, "relative copy");
	CHECK(stage_create("/", STAGE, NULL, 0) < 0 && errno == EBUSY, "staging the whole root");
	CHECK(stage_create("/var/nothere", STAGE, NULL, 0) < 0 && errno == ENOENT, "missing place");
}

int main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	mkdir("/var", 0755);
	CHECK(mkdir(BASE, 0777) == 0 || errno == EEXIST, "mkdir");
	chmod(BASE, 0777);
	CHECK(put(BASE "/orig", "base") == 0, "base file");
	CHECK(mkdir(STAGE, 0777) == 0 || errno == EEXIST, "mkdir");
	chmod(STAGE, 0777);
	CHECK(put(STAGE "/orig", "copy") == 0 && chown(STAGE "/orig", UID, GID) == 0, "copy file");

	refused();
	run();
	killed();
	return finish();
}
