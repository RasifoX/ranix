/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* asess(SPAWN): the child is inside from its first instruction, with no enter
 * call, and the caller stays out. Refusals, fd sweep, /proc introspection, and
 * teardown when pid 1 of the session dies. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdint.h>
#include <sys/ptrace.h>
#include <sys/types.h>
#include <sys/stat.h>
#include "asess_test.h"

static int slurp(const char *path, char *buf, size_t sz)
{
	int fd = open(path, O_RDONLY);
	if (fd < 0) return -1;
	ssize_t n = read(fd, buf, sz - 1);
	close(fd);
	if (n < 0) return -1;
	buf[n] = '\0';
	return (int)n;
}

static const char *const ns_kinds[] = { "pid", "net", "uts", "ipc", "mnt" };
#define NS_N 5
static char host_ns[NS_N][64];

static void read_ns(char out[NS_N][64])
{
	for (int i = 0; i < NS_N; i++) {
		char p[64];
		snprintf(p, sizeof(p), "/proc/self/ns/%s", ns_kinds[i]);
		ssize_t n = readlink(p, out[i], 63);
		out[i][n > 0 ? n : 0] = '\0';
	}
}

/* capget(0) on self: {version, pid}, then three u32 pairs */
static int caps_all_zero(void)
{
	uint32_t hdr[2] = { 0x20080522, 0 }, d[6] = { 1, 1, 1, 1, 1, 1 };
	if (syscall(SYS_capget, hdr, d)) return 0;
	return !(d[0] | d[1] | d[2] | d[3] | d[4] | d[5]);
}

static void child_body(long sfd, int other_fd, int go_rd, int ready_wr, unsigned long long want)
{
	char buf[512], ns[NS_N][64];

	CHECK(asess_self() == want && want, "id set");
	CHECK(getpid() == 1, "pid 1 of the session");
	CHECK(getuid() == UID && geteuid() == UID, "uid");
	CHECK(getgid() == GID && getegid() == GID, "gid");
	uid_t r, e, s; getresuid(&r, &e, &s);
	CHECK(s == UID, "saved uid");
	CHECK(getgroups(0, NULL) == 0, "no groups");
	CHECK(caps_all_zero(), "capabilities cleared");
	CHECK(prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0) == 1, "no_new_privs");
	CHECK(getsid(0) == 1 && getpgid(0) == 1, "own session and group");
	CHECK(getcwd(buf, sizeof(buf)) && strcmp(buf, "/") == 0, "cwd");

	CHECK(fcntl(other_fd, F_GETFD) == -1 && errno == EBADF, "unlisted fd closed");
	CHECK(fcntl((int)sfd, F_GETFD) == -1 && errno == EBADF, "sfd closed");
	CHECK(fcntl(1, F_GETFD) >= 0, "kept fd open");

	struct asess_attr a;
	memset(&a, 0, sizeof(a));
	a.size = sizeof(a);
	a.root = (unsigned long long)(unsigned long)"/";
	DENIED(asess(ASESS_CREATE, -1, &a, sizeof(a)), EPERM, "create from inside");
	DENIED(asess(ASESS_SPAWN, (int)sfd, NULL, 0), EPERM, "spawn from inside");
	struct asess_info in;
	CHECK(asess(ASESS_INFO, -1, &in, sizeof(in)) == 0 && in.id == want, "info from inside");

	read_ns(ns);
	for (int i = 0; i < NS_N; i++) {
		if (!ns[i][0] || !strcmp(ns[i], host_ns[i])) {
			printf("FAIL %s namespace is the host's: %s\n", ns_kinds[i], ns[i]);
			fails++;
		}
	}

	CHECK(slurp("/proc/self/asess/status", buf, sizeof(buf)) > 0 &&
	      strstr(buf, "state: running") && strstr(buf, "label: test"), "/proc/self/asess/status");
	CHECK(slurp("/proc/self/asess/tasks", buf, sizeof(buf)) > 0 && strstr(buf, "1\n"),
	      "tasks lists the task by its session pid");

	if (fork() == 0) { pause(); _exit(0); }   /* must die with us */

	char c = 'r';
	CHECK(write(ready_wr, &c, 1) == 1, "signal ready");
	CHECK(read(go_rd, &c, 1) == 1, "wait for the outside checks");
	_exit(fails ? 1 : 0);
}

static void refusals(void)
{
	struct asess_attr a;
	int keep[1];

	memset(&a, 0, sizeof(a));
	a.size = sizeof(a);
	a.root = (unsigned long long)(unsigned long)"/";
	DENIED(asess(ASESS_CREATE, -1, &a, sizeof(a)), EPERM, "root caller must name a non-root uid");

	a.uid = UID; a.gid = GID;
	a.flags = 1;
	DENIED(asess(ASESS_CREATE, -1, &a, sizeof(a)), EINVAL, "unknown flag");
	a.flags = 0;
	a.net_mode = 1;
	DENIED(asess(ASESS_CREATE, -1, &a, sizeof(a)), EINVAL, "unknown net mode");
	a.net_mode = 0;
	DENIED(asess(ASESS_CREATE, -1, &a, sizeof(a) - 1), EINVAL, "short attr");
	a.root = (unsigned long long)(unsigned long)"bin";
	DENIED(asess(ASESS_CREATE, -1, &a, sizeof(a)), EINVAL, "relative root");
	a.root = (unsigned long long)(unsigned long)"/bin/sh";
	DENIED(asess(ASESS_CREATE, -1, &a, sizeof(a)), ENOTDIR, "root is not a directory");
	a.root = (unsigned long long)(unsigned long)"/proc/self";
	DENIED(asess(ASESS_CREATE, -1, &a, sizeof(a)), EINVAL, "root that cannot stay cached");

	DENIED(asess(ASESS_SPAWN, 0, NULL, 0), EBADF, "spawn on a non-session fd");

	keep[0] = open("/bin/sh", O_RDONLY);
	CHECK(keep[0] >= 0, "open a regular file");
	long sfd = sess_create("/", UID, keep, 1, 0);
	CHECK(sfd >= 0, "create with a file to keep");
	DENIED(sess_spawn(sfd), EINVAL, "regular file in keep_fds");
	close((int)sfd);
	close(keep[0]);

	keep[0] = open("/bin", O_RDONLY | O_DIRECTORY);
	sfd = sess_create("/", UID, keep, 1, 0);
	DENIED(sess_spawn(sfd), EINVAL, "directory in keep_fds");
	close((int)sfd);
	close(keep[0]);

	keep[0] = 77;
	sfd = sess_create("/", UID, keep, 1, 0);
	DENIED(sess_spawn(sfd), EBADF, "closed fd in keep_fds");
	close((int)sfd);

	/* the sfd itself must not cross */
	sfd = sess_create("/", UID, NULL, 0, 0);
	keep[0] = (int)sfd;
	long sfd2 = sess_create("/", UID, keep, 1, 0);
	DENIED(sess_spawn(sfd2), EINVAL, "sfd in keep_fds");
	close((int)sfd2);
	close((int)sfd);

	pid_t t = fork();
	if (t == 0) {
		ptrace(PTRACE_TRACEME, 0, 0, 0);
		long s = sess_create("/", UID, NULL, 0, 0);
		_exit(s >= 0 && sess_spawn(s) == -1 && errno == EPERM ? 0 : 1);
	}
	sess_reap(t);

	/* an unprivileged caller may use its own ids and no other */
	t = fork();
	if (t == 0) {
		if (setgid(GID) || setuid(UID)) _exit(9);
		long s = sess_create("/", UID + 1, NULL, 0, 0);
		if (s != -1 || errno != EPERM) _exit(1);
		struct asess_attr u;
		memset(&u, 0, sizeof(u));
		u.size = sizeof(u);
		u.root = (unsigned long long)(unsigned long)"/";
		s = asess(ASESS_CREATE, -1, &u, sizeof(u));
		if (s < 0) _exit(2);
		long p = sess_spawn(s);
		if (p < 0) _exit(3);
		if (p == 0) _exit(asess_self() && getuid() == UID ? 0 : 1);
		int st = 1;
		waitpid(p, &st, 0);
		_exit(WIFEXITED(st) && WEXITSTATUS(st) == 0 ? 0 : 4);
	}
	sess_reap(t);
}

int main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	CHECK(asess_self() == 0, "not in a session");
	read_ns(host_ns);
	refusals();

	int go[2], ready[2], other[2];
	CHECK(pipe(go) == 0 && pipe(ready) == 0 && pipe(other) == 0, "pipes");
	int keep[] = { 1, go[0], ready[1] };

	long sfd = sess_create("/", UID, keep, 3, 0);
	CHECK(sfd >= 0, "create");
	CHECK((fcntl((int)sfd, F_GETFD) & FD_CLOEXEC) != 0, "sfd is close-on-exec");
	struct asess_info info;
	CHECK(asess(ASESS_INFO, (int)sfd, &info, sizeof(info)) == 0 &&
	      info.state == ASESS_STATE_CREATED && info.tasks == 0, "info before spawn");

	long pid = sess_must_spawn(sfd);
	if (pid == 0) child_body(sfd, other[0], go[0], ready[1], info.id);

	CHECK(asess_self() == 0, "caller stays outside");
	CHECK(pid > 1, "spawn returns the host pid");
	DENIED(sess_spawn(sfd), EBUSY, "second spawn");

	char c, path[64], buf[512];
	CHECK(read(ready[0], &c, 1) == 1, "session ready");

	snprintf(path, sizeof(path), "/proc/asess/%llu/status", info.id);
	CHECK(slurp(path, buf, sizeof(buf)) > 0 && strstr(buf, "state: running") &&
	      strstr(buf, "uid: 1000") && strstr(buf, "tasks: 2"), "/proc/asess status");
	snprintf(path, sizeof(path), "/proc/%ld/asess/status", pid);
	CHECK(slurp(path, buf, sizeof(buf)) > 0 && strstr(buf, "state: running"), "/proc/<pid>/asess");
	snprintf(path, sizeof(path), "/proc/asess/%llu/tasks", info.id);
	char want[32];
	snprintf(want, sizeof(want), "%ld\n", pid);
	CHECK(slurp(path, buf, sizeof(buf)) > 0 && strstr(buf, want), "tasks by host pid");

	CHECK(write(go[1], "g", 1) == 1, "release");
	sess_reap((pid_t)pid);

	/* pid 1 is gone, so the namespace kills the member it left behind */
	int gone = 0;
	for (int i = 0; i < 300 && !gone; i++) {
		CHECK(asess(ASESS_INFO, (int)sfd, &info, sizeof(info)) == 0, "info after exit");
		gone = info.tasks == 0;
		if (!gone) usleep(10000);
	}
	CHECK(gone, "members die with pid 1");
	CHECK(info.state == ASESS_STATE_DEAD, "state dead");

	close((int)sfd);
	snprintf(path, sizeof(path), "/proc/asess/%llu/status", info.id);
	CHECK(access(path, F_OK) == -1, "gone from /proc once the last reference drops");

	/* a session does not outlive its last sfd */
	sfd = sess_create("/", UID, NULL, 0, 0);
	pid = sess_spawn(sfd);
	if (pid == 0) { for (;;) pause(); }
	CHECK(pid > 0, "spawn to orphan");
	close((int)sfd);
	int st = 0;
	CHECK(waitpid((pid_t)pid, &st, 0) == pid && WIFSIGNALED(st) && WTERMSIG(st) == 9,
	      "closing the last sfd kills the session");
	return finish();
}
