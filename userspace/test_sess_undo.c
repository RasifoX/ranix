/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* ranix-sess -S end to end: what a session writes is staged, commit applies it,
 * rollback leaves no trace, snapshots restore, and a killed session or a cut
 * commit leaves the directory either untouched or finishable. */
#define _GNU_SOURCE
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <sys/stat.h>
#include "asess_test.h"

#define SD   "/tmp/rsd"        /* RANIX_SESS_DIR */
#define B    "/var/ub"         /* the staged directory */
#define SESS "/bin/ranix-sess"

static char out[4096];

/* run argv with stdout to a pipe; out gets what it printed; returns the status */
static int runv(const char *env, char *const argv[])
{
	int p[2];
	if (pipe(p)) return -1;
	pid_t pid = fork();
	if (pid == 0) {
		if (env) putenv((char *)env);
		dup2(p[1], 1);
		close(p[0]); close(p[1]);
		int nul = open("/dev/null", O_RDWR);
		dup2(nul, 0);
		dup2(getenv("SESS_ERR_OUT") ? 1 : nul, 2);   /* a session keeps only pipes, ttys, /dev/null */
		setenv("RANIX_SESS_DIR", SD, 1);
		execv(argv[0], argv);
		_exit(127);
	}
	close(p[1]);
	size_t n = 0;
	ssize_t k;
	while (n < sizeof(out) - 1 && (k = read(p[0], out + n, sizeof(out) - 1 - n)) > 0) n += (size_t)k;
	out[n] = 0;
	close(p[0]);
	int st = 0;
	waitpid(pid, &st, 0);
	return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
}

static int sess(const char *env, ...)
{
	char *argv[32];
	int n = 0;
	argv[n++] = SESS;
	va_list ap;
	va_start(ap, env);
	char *a;
	while (n < 31 && (a = va_arg(ap, char *))) argv[n++] = a;
	va_end(ap);
	argv[n] = NULL;
	return runv(env, argv);
}

/* the agent: a shell command inside a staged session */
static int agent(const char *name, const char *cmd)
{
	int r = sess("SESS_ERR_OUT=1", "run", "-r", "/", "-u", "1000", "-g", "1000", "-S", B, "-n", name, "--",
	             "/bin/sh", "-c", cmd, (char *)NULL);
	if (r) printf("agent %s rc=%d: %.200s\n", name, r, out);
	return r;
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

static int absent(const char *p)
{
	struct stat st;
	return lstat(p, &st) < 0 && errno == ENOENT;
}

/* a fingerprint of the tree: names, kinds, modes, sizes and bytes */
static unsigned long long h;
static void mix(const void *p, size_t n)
{
	const unsigned char *b = p;
	for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ULL; }
}
static int ncmp(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
static void fp(const char *dir)
{
	DIR *d = opendir(dir);
	if (!d) { mix("?", 1); return; }
	char *names[256];
	int n = 0;
	struct dirent *de;
	while ((de = readdir(d)) && n < 256)
		if (strcmp(de->d_name, ".") && strcmp(de->d_name, "..")) names[n++] = strdup(de->d_name);
	closedir(d);
	qsort(names, (size_t)n, sizeof(char *), ncmp);
	for (int i = 0; i < n; i++) {
		char p[512];
		snprintf(p, sizeof(p), "%s/%s", dir, names[i]);
		struct stat st;
		mix(names[i], strlen(names[i]) + 1);
		if (lstat(p, &st) == 0) {
			unsigned m = st.st_mode;
			mix(&m, sizeof(m));
			if (S_ISDIR(st.st_mode)) fp(p);
			else if (S_ISREG(st.st_mode)) {
				char b[512];
				int fd = open(p, O_RDONLY);
				ssize_t k;
				while (fd >= 0 && (k = read(fd, b, sizeof(b))) > 0) mix(b, (size_t)k);
				if (fd >= 0) close(fd);
			} else if (S_ISLNK(st.st_mode)) {
				char b[256];
				ssize_t k = readlink(p, b, sizeof(b));
				if (k > 0) mix(b, (size_t)k);
			}
		}
		free(names[i]);
	}
}
static unsigned long long tree(void) { h = 1469598103934665603ULL; fp(B); return h; }

static void reset(void)
{
	runv(NULL, (char *[]){ "/bin/rm", "-rf", B, SD, NULL });
	CHECK(mkdir(B, 0755) == 0 && mkdir(B "/d", 0755) == 0, "mkdir");
	CHECK(put(B "/f", "one") == 0 && put(B "/d/g", "two") == 0 && put(B "/rmme", "x") == 0, "files");
	static const char *const own[] = { B, B "/d", B "/f", B "/d/g", B "/rmme" };
	for (unsigned i = 0; i < sizeof(own) / sizeof(own[0]); i++)
		CHECK(chown(own[i], 1000, 1000) == 0, "chown");
}

#define WORK "echo changed > " B "/f; rm " B "/rmme; mkdir -p " B "/n/m; echo new > " B "/n/m/h; " \
             "mv " B "/d " B "/d2; ln -s /etc " B "/esc; echo s > " B "/su; chmod 4755 " B "/su || chmod 755 " B "/su"

static void commit(void)
{
	reset();
	unsigned long long before = tree();
	CHECK(agent("c1", WORK) == 0, "agent ran");
	CHECK(tree() == before, "nothing reaches the directory before commit");
	CHECK(sess(NULL, "diff", "c1", (char *)NULL) == 0, "diff");
	if (!(strstr(out, "M f\n") && strstr(out, "D rmme\n") && strstr(out, "A n/m/h\n") &&
	      strstr(out, "D d/g\n") && strstr(out, "A d2/g\n"))) printf("diff said:\n%s", out);
	CHECK(strstr(out, "M f\n") && strstr(out, "D rmme\n") && strstr(out, "A n/m/h\n") &&
	      strstr(out, "D d/g\n") && strstr(out, "A d2/g\n"), "diff lists the changes");
	CHECK(sess(NULL, "commit", "c1", (char *)NULL) == 0, "commit");
	CHECK(got(B "/f", "changed\n") && absent(B "/rmme") && got(B "/n/m/h", "new\n"), "committed");
	CHECK(absent(B "/d") && got(B "/d2/g", "two"), "rename committed");
	char l[16] = { 0 };
	CHECK(readlink(B "/esc", l, sizeof(l)) == 4 && !strcmp(l, "/etc"), "symlink committed as a symlink");
	struct stat st;
	CHECK(stat(B "/su", &st) == 0 && (st.st_mode & 07777) == 0755 && st.st_uid == 1000,
	      "set-user-ID dropped, owner kept");
	CHECK(absent(SD "/c1"), "commit leaves no state");
}

static void rollback(void)
{
	reset();
	unsigned long long before = tree();
	CHECK(agent("r1", WORK) == 0, "agent ran");
	CHECK(sess(NULL, "rollback", "r1", (char *)NULL) == 0, "rollback");
	CHECK(tree() == before, "rollback: the directory is as before");
	CHECK(absent(SD "/r1"), "rollback leaves no state");
	CHECK(sess(NULL, "commit", "r1", (char *)NULL) != 0, "nothing left to commit");
	CHECK(tree() == before, "still as before");
}

static void snapshots(void)
{
	reset();
	CHECK(agent("s1", "echo v1 > " B "/f") == 0, "step 1");
	CHECK(sess(NULL, "snapshot", "s1", (char *)NULL) == 0 && !strcmp(out, "1\n"), "snapshot 1");
	CHECK(agent("s1", "echo v2 > " B "/f; echo junk > " B "/j; rm -r " B "/d") == 0, "step 2 continues");
	CHECK(sess(NULL, "rollback", "s1", "1", (char *)NULL) == 0, "rollback to 1");
	CHECK(agent("s1", "cat " B "/f; test -e " B "/j || echo nojunk") == 0 &&
	      strstr(out, "v1\nnojunk\n"), "the session sees snapshot 1");
	CHECK(sess(NULL, "commit", "s1", (char *)NULL) == 0, "commit");
	CHECK(got(B "/f", "v1\n") && absent(B "/j") && got(B "/d/g", "two"), "snapshot 1 committed");

	reset();
	unsigned long long before = tree();
	CHECK(agent("s2", WORK) == 0, "agent ran");
	CHECK(sess(NULL, "rollback", "s2", "0", (char *)NULL) == 0, "rollback to 0");
	CHECK(sess(NULL, "diff", "s2", (char *)NULL) == 0 && out[0] == 0, "snapshot 0 is the start");
	CHECK(sess(NULL, "commit", "s2", (char *)NULL) == 0 && tree() == before, "an empty commit");
}

/* the supervisor killed while the agent works: nothing reaches the directory */
static void killed(void)
{
	reset();
	unsigned long long before = tree();
	pid_t pid = fork();
	if (pid == 0) {
		int nul = open("/dev/null", O_RDWR);
		dup2(nul, 0); dup2(nul, 1); dup2(nul, 2);
		setenv("RANIX_SESS_DIR", SD, 1);
		execl(SESS, SESS, "run", "-r", "/", "-u", "1000", "-g", "1000", "-S", B, "-n", "k1", "--",
		      "/bin/sh", "-c", "echo partial > " B "/f; rm " B "/rmme; sleep 30", (char *)NULL);
		_exit(127);
	}
	int seen = 0;
	for (int i = 0; i < 200 && !seen; i++) {
		seen = got(SD "/k1/stage/f", "partial\n") && absent(SD "/k1/stage/rmme");
		if (!seen) usleep(50000);
	}
	CHECK(seen, "the agent wrote to the stage");
	CHECK(kill(pid, SIGKILL) == 0, "kill the supervisor");
	int st = 0;
	waitpid(pid, &st, 0);
	CHECK(tree() == before, "the directory is untouched after the kill");
	int ok = 0;
	for (int i = 0; i < 600 && !ok; i++) {
		ok = sess(NULL, "rollback", "k1", (char *)NULL) == 0;
		if (!ok) usleep(50000);
	}
	CHECK(ok && absent(SD "/k1") && tree() == before, "rollback after the kill");
}

/* a commit cut after one change is finished by the next */
static void cut(void)
{
	reset();
	CHECK(agent("x1", WORK) == 0, "agent ran");
	int r = sess("RANIX_SESS_FAIL_AFTER=1", "commit", "x1", (char *)NULL);
	CHECK(r == 99, "commit cut");
	CHECK(!absent(SD "/x1"), "the stage stays after a cut");
	CHECK(sess(NULL, "commit", "x1", (char *)NULL) == 0, "commit again");
	unsigned long long finished = tree();
	reset();
	CHECK(agent("x2", WORK) == 0 && sess(NULL, "commit", "x2", (char *)NULL) == 0, "uncut commit");
	CHECK(tree() == finished, "cut and finished equals uncut");
}

static void conflict(void)
{
	reset();
	CHECK(agent("q1", "echo agent > " B "/f; echo also > " B "/new") == 0, "agent ran");
	CHECK(put(B "/f", "human") == 0, "a human edits meanwhile");
	CHECK(sess(NULL, "commit", "q1", (char *)NULL) == 1 && strstr(out, "conflict f\n"), "conflict reported");
	CHECK(got(B "/f", "human") && absent(B "/new"), "nothing applied");
	CHECK(sess(NULL, "commit", "q1", "-f", (char *)NULL) == 0 && got(B "/f", "agent\n"), "forced");
}

/* the stage's symlinks are never followed by commit */
static void symlinks(void)
{
	reset();
	CHECK(mkdir("/var/victim", 0777) == 0 || errno == EEXIST, "victim");
	chmod("/var/victim", 0777);
	CHECK(put("/var/victim/v", "safe") == 0, "victim file");
	CHECK(agent("y1", "rm -r " B "/d; ln -s /var/victim " B "/d; echo pwn > " B "/d/v 2>/dev/null; "
	          "echo pwn > " B "/d/new 2>/dev/null; true") == 0, "agent ran");
	CHECK(sess(NULL, "commit", "y1", (char *)NULL) == 0, "commit");
	char l[32] = { 0 };
	CHECK(readlink(B "/d", l, sizeof(l)) > 0 && !strcmp(l, "/var/victim"), "the link itself is committed");
	CHECK(got("/var/victim/v", "safe") && absent("/var/victim/new"), "nothing written through it");

	/* a human swaps a directory for a symlink before the commit */
	reset();
	CHECK(agent("y2", "echo agent > " B "/d/g") == 0, "agent ran");
	runv(NULL, (char *[]){ "/bin/rm", "-rf", B "/d", NULL });
	CHECK(symlink("/var/victim", B "/d") == 0, "swap");
	CHECK(sess(NULL, "commit", "y2", (char *)NULL) == 1 && strstr(out, "conflict d/g\n"), "conflict");
	CHECK(sess(NULL, "commit", "y2", "-f", (char *)NULL) != 0, "forced commit fails at the link");
	CHECK(absent("/var/victim/g") && got("/var/victim/v", "safe"), "nothing written through it");
	CHECK(sess(NULL, "rollback", "y2", (char *)NULL) == 0, "drop");
	runv(NULL, (char *[]){ "/bin/rm", "-rf", "/var/victim", NULL });
}

/* from inside, the state is out of sight and the session cannot decide */
static void inside(void)
{
	reset();
	CHECK(agent("z1", "echo in > " B "/f") == 0, "agent ran");
	CHECK(agent("z2", SESS " commit z1; echo rc=$?") == 0 && strstr(out, "rc=125"), "commit from inside fails");
	CHECK(sess(NULL, "rollback", "z2", (char *)NULL) == 0, "drop z2");
	CHECK(sess(NULL, "commit", "z1", (char *)NULL) == 0 && got(B "/f", "in\n"), "z1 still commits");
}

int main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	mkdir("/var", 0755);
	commit();
	rollback();
	snapshots();
	killed();
	cut();
	conflict();
	symlinks();
	inside();
	runv(NULL, (char *[]){ "/bin/rm", "-rf", B, SD, NULL });
	return finish();
}
