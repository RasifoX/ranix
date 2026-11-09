/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* An AF_UNIX path bind makes a file, so it obeys the rules for making one:
 *  - an existing name is EADDRINUSE: a plain file, or the socket file a closed
 *    socket left behind (a server unlinks it before binding again);
 *  - a directory the caller may not write is EACCES, a missing one ENOENT;
 *  - a failed bind leaves the socket unbound (it can bind elsewhere) and puts
 *    no name in the socket table (connect to the name still fails);
 *  - a good bind makes a socket file that connect reaches.
 * Otherwise a user could take a service's socket name while it restarts.
 * Prints FAIL lines, then OK or FAIL <n>. Runs as root. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>

#define PLAIN  "/tmp/ub_plain"
#define LEFT   "/tmp/ub_left.sock"
#define GOOD   "/tmp/ub_good.sock"
#define RDIR   "/tmp/ub_rootdir"
#define USER   4321

static int fails;
#define CHECK(c, what) do { if (!(c)) { printf("FAIL %s (errno=%d)\n", what, errno); fflush(stdout); fails++; } } while (0)

static socklen_t fill(struct sockaddr_un *u, const char *path)
{
	memset(u, 0, sizeof(*u));
	u->sun_family = AF_UNIX;
	strncpy(u->sun_path, path, sizeof(u->sun_path) - 1);
	return sizeof(*u);
}

/* bind a new socket to path; returns the socket (bound) or -1 with errno */
static int bound(const char *path)
{
	struct sockaddr_un u;
	socklen_t l = fill(&u, path);
	int s = socket(AF_UNIX, SOCK_STREAM, 0);
	if (s < 0) return -1;
	if (bind(s, (struct sockaddr *)&u, l) == 0) return s;
	int e = errno;
	close(s);
	errno = e;
	return -1;
}

static int dial(const char *path)
{
	struct sockaddr_un u;
	socklen_t l = fill(&u, path);
	int s = socket(AF_UNIX, SOCK_STREAM, 0);
	int r = connect(s, (struct sockaddr *)&u, l);
	int e = errno;
	close(s);
	errno = e;
	return r;
}

static void existing(void)
{
	int fd = open(PLAIN, O_CREAT | O_WRONLY, 0644);
	if (fd >= 0) close(fd);
	errno = 0;
	int s = bound(PLAIN);
	CHECK(s < 0 && errno == EADDRINUSE, "bind over an existing plain file");
	if (s >= 0) close(s);
	struct stat st;
	CHECK(stat(PLAIN, &st) == 0 && S_ISREG(st.st_mode), "the plain file was replaced");
	errno = 0;
	CHECK(dial(PLAIN) == -1, "connect reached a name whose bind failed");

	/* a failed bind leaves the socket free to bind elsewhere */
	struct sockaddr_un u;
	socklen_t l = fill(&u, PLAIN);
	int t = socket(AF_UNIX, SOCK_STREAM, 0);
	CHECK(bind(t, (struct sockaddr *)&u, l) == -1, "second socket bound the plain file");
	l = fill(&u, GOOD);
	CHECK(bind(t, (struct sockaddr *)&u, l) == 0, "a socket whose bind failed cannot bind again");
	close(t);
	unlink(GOOD);
	unlink(PLAIN);
}

static void leftover(void)
{
	int s = bound(LEFT);
	CHECK(s >= 0, "bind a fresh name");
	struct stat st;
	CHECK(stat(LEFT, &st) == 0 && S_ISSOCK(st.st_mode), "bind made no socket file");
	CHECK(s >= 0 && listen(s, 1) == 0 && dial(LEFT) == 0, "connect to a bound name");
	if (s >= 0) close(s);
	errno = 0;
	s = bound(LEFT);
	CHECK(s < 0 && errno == EADDRINUSE, "bind over a closed socket's file without unlink");
	if (s >= 0) close(s);
	unlink(LEFT);
	s = bound(LEFT);
	CHECK(s >= 0, "bind after unlink");
	if (s >= 0) close(s);
	unlink(LEFT);
}

static void where(void)
{
	errno = 0;
	int s = bound("/tmp/ub_no_such_dir/x.sock");
	CHECK(s < 0 && errno == ENOENT, "bind into a missing directory");
	if (s >= 0) close(s);

	mkdir(RDIR, 0755);
	if (chown(RDIR, 0, 0) || chmod(RDIR, 0755)) { puts("FAIL setup root dir"); fails++; return; }
	fflush(stdout);
	pid_t c = fork();
	if (c == 0) {
		if (setgroups(0, NULL) || setgid(USER) || setuid(USER)) _exit(2);
		errno = 0;
		int t = bound(RDIR "/svc.sock");
		_exit(t < 0 && errno == EACCES ? 0 : 1);
	}
	int st = 0;
	waitpid(c, &st, 0);
	CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "a user bound a name in a root 0755 directory");
	struct stat sb;
	CHECK(stat(RDIR "/svc.sock", &sb) == -1, "the refused bind left a file");
	CHECK(dial(RDIR "/svc.sock") == -1, "the refused bind left a name connect reaches");
	unlink(RDIR "/svc.sock");
	rmdir(RDIR);
}

int main(void)
{
	if (geteuid() != 0) { puts("FAIL run as root"); return 0; }
	setvbuf(stdout, NULL, _IONBF, 0);
	unlink(PLAIN); unlink(LEFT); unlink(GOOD);
	existing();
	leftover();
	where();
	if (fails) printf("FAIL %d\n", fails); else puts("OK");
	return 0;
}
