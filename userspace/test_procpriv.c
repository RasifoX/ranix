/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* /proc/<pid> as Linux guards it, and fs.suid_dumpable.
 * Every /proc/<pid> entry belongs to the task's euid/egid while it is dumpable
 * (dumpable 1) and to root otherwise (Linux task_dump_owner). fd/ and fdinfo/
 * are 0500, environ, auxv and syscall 0400, mem 0600: another user, and the
 * same user when the task is not dumpable, gets EACCES from the directory
 * itself, not a list of fd numbers and ENOENT. What passes the mode is still
 * held to ptrace_may_access: an fd link, exe/cwd/root, environ, maps, auxv and
 * mem of a task one may not trace are EACCES. status, stat and cmdline stay
 * open to all. A dumpable task of one's own uid is open: its fds, environ (the
 * environment it was started with), maps, auxv.
 * fs.suid_dumpable (0 by default, 0644 root): 0 a task that changed its ids is
 * not dumpable; 1 it stays dumpable; 2 it is dumpable "as root" (PR_GET_DUMPABLE
 * 2): still not open to its user, and its core goes only to an absolute
 * core_pattern, owned by root. Other values EINVAL. exec with euid != ruid
 * gives suid_dumpable too. A task that is not dumpable leaves no core.
 * Runs as root. "nosysctl": leave the machine's knobs alone (a Linux host).
 * Prints FAIL lines, then OK or FAIL. */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static const char *self_exe;
#define SECRET "/tmp/procpriv-secret"
#define PUBLIC "/tmp/procpriv-public"
static int yama;   /* Linux Yama ptrace_scope: mem (an attach) is refused to non-ancestors */
#define ENVMARK "PROCPRIV_ENV=orca-marker"

/* errno of an operation on /proc/<pid>/<name>, 0 if it went through */
enum { OP_OPEN, OP_OPENDIR, OP_READLINK, OP_READ };
static int try_op(int op, pid_t pid, const char *name)
{
	char path[96], buf[512];
	snprintf(path, sizeof path, "/proc/%d/%s", (int)pid, name);
	errno = 0;
	switch (op) {
	case OP_OPENDIR: {
		DIR *d = opendir(path);
		if (!d) return errno;
		int n = 0;
		while (readdir(d)) n++;
		closedir(d);
		return n > 2 ? 0 : -1;          /* opened, and fds were listed */
	}
	case OP_READLINK:
		return readlink(path, buf, sizeof buf) >= 0 ? 0 : errno;
	case OP_OPEN: {
		int fd = open(path, O_RDONLY);
		if (fd < 0) return errno;
		close(fd);
		return 0;
	}
	default: {
		int fd = open(path, O_RDONLY);
		if (fd < 0) return errno;
		ssize_t n = read(fd, buf, sizeof buf);
		int e = errno;
		close(fd);
		return n > 0 ? 0 : (n < 0 ? e : -1);
	}
	}
}

static const char *ename(int e)
{
	return e == 0 ? "allowed" : e == -1 ? "empty" : strerror(e);
}

/* in a child of uid/gid id: every refused entry EACCES, every open one allowed */
/* fdn: root's 0600 secret in the target, pub: a 0644 file (open through the
 * link goes on to the file's own mode) */
static void look(const char *who, pid_t t, int fdn, int pub, int open_to_me, uid_t id)
{
	pid_t o = fork();
	if (o == 0) {
		if (setgid(id) || setuid(id)) _exit(100);
		char fdp[32], fip[40], pubp[32];
		snprintf(fdp, sizeof fdp, "fd/%d", fdn);
		snprintf(pubp, sizeof pubp, "fd/%d", pub);
		snprintf(fip, sizeof fip, "fdinfo/%d", fdn);
		static const struct { int op; const char *name; } guarded[] = {
			{ OP_OPENDIR, "fd" }, { OP_OPENDIR, "fdinfo" }, { OP_READ, "environ" },
			{ OP_READ, "maps" }, { OP_READ, "auxv" }, { OP_OPEN, "mem" },
			{ OP_READLINK, "exe" }, { OP_READLINK, "cwd" }, { OP_READLINK, "root" },
		};
		int want = open_to_me ? 0 : EACCES;
		for (unsigned i = 0; i < sizeof guarded / sizeof guarded[0]; i++) {
			if (yama && strcmp(guarded[i].name, "mem") == 0 && id != 0) continue;
			int e = try_op(guarded[i].op, t, guarded[i].name);
			CHECK(e == want, "%s: /proc/<pid>/%s as uid %u: %s, want %s", who, guarded[i].name, id, ename(e), ename(want));
		}
		int e = try_op(OP_READLINK, t, fdp);
		CHECK(e == want, "%s: readlink /proc/<pid>/%s as uid %u: %s, want %s", who, fdp, id, ename(e), ename(want));
		e = try_op(OP_OPEN, t, pubp);
		CHECK(e == want, "%s: open /proc/<pid>/%s (a 0644 file) as uid %u: %s, want %s", who, pubp, id, ename(e), ename(want));
		e = try_op(OP_READ, t, fip);
		CHECK(e == want, "%s: read /proc/<pid>/%s as uid %u: %s, want %s", who, fip, id, ename(e), ename(want));
		static const char *open_all[] = { "status", "stat", "cmdline", "statm", "comm" };
		for (unsigned i = 0; i < sizeof open_all / sizeof open_all[0]; i++) {
			e = try_op(OP_READ, t, open_all[i]);
			CHECK(e == 0, "%s: /proc/<pid>/%s as uid %u: %s, want it readable", who, open_all[i], id, ename(e));
		}
		if (open_to_me == 2) {   /* exec'd with ENVMARK */
			char path[64], env[4096];
			snprintf(path, sizeof path, "/proc/%d/environ", (int)t);
			int fd = open(path, O_RDONLY);
			ssize_t n = fd >= 0 ? read(fd, env, sizeof env - 1) : -1;
			if (fd >= 0) close(fd);
			int found = 0;
			for (ssize_t i = 0; n > 0 && i < n; i += (ssize_t)strlen(env + i) + 1) {
				env[n] = 0;
				if (strcmp(env + i, ENVMARK) == 0) found = 1;
			}
			CHECK(found, "%s: /proc/<pid>/environ has no %s (read %zd)", who, ENVMARK, n);
		}
		_exit(fails);
	}
	int st = 0;
	waitpid(o, &st, 0);
	CHECK(WIFEXITED(st) && WEXITSTATUS(st) != 100, "%s: the observer could not become uid %u", who, id);
	if (WIFEXITED(st) && WEXITSTATUS(st) != 100) fails += WEXITSTATUS(st);
}

static void owner_is(const char *who, pid_t t, const char *name, uid_t uid, mode_t mode)
{
	char path[64];
	struct stat sb;
	snprintf(path, sizeof path, "/proc/%d%s%s", (int)t, *name ? "/" : "", name);
	if (stat(path, &sb) != 0) { CHECK(0, "%s: stat %s: %s", who, path, strerror(errno)); return; }
	CHECK(sb.st_uid == uid, "%s: %s belongs to uid %u, want %u", who, *name ? name : "/proc/<pid>", sb.st_uid, uid);
	if (mode) CHECK((sb.st_mode & 07777) == mode, "%s: %s mode %o, want %o", who, *name ? name : "/proc/<pid>", sb.st_mode & 07777, mode);
}

/* A child that starts as root, opens the secret, and becomes uid id, then (exec)
 * runs us again with ENVMARK in its environment, or (no exec) waits as it is.
 * *fdn: the secret's fd in it, *pub the public file's; *dump: its PR_GET_DUMPABLE. */
static pid_t start_target(uid_t id, int exec_again, int *fdn, int *pub, int *dump)
{
	int p[2];
	if (pipe(p)) return -1;
	pid_t t = fork();
	if (t == 0) {
		close(p[0]);
		int fd = open(SECRET, O_RDONLY), pf = open(PUBLIC, O_RDONLY);
		if (fd < 0 || pf < 0 || setgid(id) || setuid(id)) _exit(9);
		if (exec_again) {
			char a1[16], a2[16], a3[16];
			snprintf(a1, sizeof a1, "%d", p[1]);
			snprintf(a2, sizeof a2, "%d", fd);
			snprintf(a3, sizeof a3, "%d", pf);
			char *argv[] = { (char *)self_exe, "--wait", a1, a2, a3, NULL };
			char *envp[] = { "PATH=/bin", ENVMARK, NULL };
			execve(self_exe, argv, envp);
			_exit(10);
		}
		int m[3] = { fd, prctl(PR_GET_DUMPABLE), pf };
		if (write(p[1], m, sizeof m) != sizeof m) _exit(11);
		pause();
		_exit(0);
	}
	close(p[1]);
	int m[3] = { -1, -1, -1 };
	if (read(p[0], m, sizeof m) != sizeof m) { close(p[0]); kill(t, SIGKILL); waitpid(t, NULL, 0); return -1; }
	close(p[0]);
	*fdn = m[0]; *dump = m[1]; *pub = m[2];
	return t;
}

static void stop(pid_t t) { kill(t, SIGKILL); waitpid(t, NULL, 0); }

static int read_knob(const char *path, char *out, size_t n)
{
	int fd = open(path, O_RDONLY);
	if (fd < 0) return -errno;
	ssize_t r = read(fd, out, n - 1);
	close(fd);
	if (r < 0) return -1;
	out[r] = 0;
	return 0;
}

static int write_knob(const char *path, const char *v)
{
	int fd = open(path, O_WRONLY);
	if (fd < 0) return errno;
	ssize_t r = write(fd, v, strlen(v));
	int e = errno;
	close(fd);
	return r == (ssize_t)strlen(v) ? 0 : e;
}

/* knob: suid_dumpable as found (0 on a fresh Ranix; Ubuntu sets 2) */
static void guarded_proc(int knob)
{
	int fdn, pub, dump;
	/* root's secret held by a task that dropped to 65534: not dumpable. Its
	 * files and fd/ are root's; the world-readable directories (itself,
	 * fdinfo/) stay its euid's, as Linux keeps them for ps */
	pid_t t = start_target(65534, 0, &fdn, &pub, &dump);
	if (t < 0) { CHECK(0, "the dropping target did not start"); return; }
	CHECK(dump == knob, "a task that setuid() from root is dumpable %d, want %d (suid_dumpable)", dump, knob);
	if (knob != 1) {
		owner_is("not dumpable", t, "", 65534, 0555);
		owner_is("not dumpable", t, "fd", 0, 0500);
		owner_is("not dumpable", t, "fdinfo", 65534, 0555);
		owner_is("not dumpable", t, "environ", 0, 0400);
		owner_is("not dumpable", t, "status", 0, 0444);
		look("not dumpable, same uid", t, fdn, pub, 0, 65534);
		look("not dumpable, root", t, fdn, pub, 1, 0);
	}
	stop(t);

	/* the same, exec'd as 65534: dumpable again, its own */
	t = start_target(65534, 1, &fdn, &pub, &dump);
	if (t < 0) { CHECK(0, "the exec'd target did not start"); return; }
	CHECK(dump == 1, "a task exec'd as 65534 is dumpable %d, want 1", dump);
	owner_is("dumpable", t, "", 65534, 0555);
	owner_is("dumpable", t, "fd", 65534, 0500);
	owner_is("dumpable", t, "auxv", 65534, 0400);
	owner_is("dumpable", t, "mem", 65534, 0600);
	owner_is("dumpable", t, "fdinfo", 65534, 0555);
	look("dumpable, same uid", t, fdn, pub, 2, 65534);
	look("dumpable, other uid", t, fdn, pub, 0, 65533);
	stop(t);

	/* exec with euid != ruid: dumpable is suid_dumpable */
	pid_t c = fork();
	if (c == 0) {
		if (setresuid(65534, 0, 0)) _exit(9);
		char *argv[] = { (char *)self_exe, "--dumpable", NULL };
		execv(self_exe, argv);
		_exit(10);
	}
	int st = 0;
	waitpid(c, &st, 0);
	CHECK(WIFEXITED(st) && WEXITSTATUS(st) == knob, "exec with ruid 65534 euid 0: dumpable %d, want %d (suid_dumpable)", WIFEXITED(st) ? WEXITSTATUS(st) : -1, knob);

	/* a task that made itself not dumpable still lists and follows its own fds
	 * (Linux proc_fd_permission: its own thread group); its 0400 files are root's */
	c = fork();
	if (c == 0) {
		if (setgid(65534) || setuid(65534)) _exit(100);
		prctl(PR_SET_DUMPABLE, 0, 0, 0, 0);
		pid_t me = getpid();
		int bad = 0;
		if (try_op(OP_OPENDIR, me, "fd") != 0) bad |= 1;
		if (try_op(OP_READLINK, me, "fd/1") != 0) bad |= 2;
		if (try_op(OP_READ, me, "maps") != 0) bad |= 4;
		if (try_op(OP_READLINK, me, "exe") != 0) bad |= 8;
		if (try_op(OP_OPEN, me, "environ") != EACCES) bad |= 16;
		_exit(bad);
	}
	waitpid(c, &st, 0);
	CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "a task not dumpable by its own prctl, on its own /proc: 0x%x (1 fd/ not listed, 2 fd/1 not read, 4 maps, 8 exe, 16 environ not EACCES)", WIFEXITED(st) ? WEXITSTATUS(st) : -1);

	char env[4096];
	int fd = open("/proc/self/environ", O_RDONLY);
	ssize_t n = fd >= 0 ? read(fd, env, sizeof env) : -1;
	if (fd >= 0) close(fd);
	CHECK(n > 0, "/proc/self/environ: %zd (%s)", n, strerror(errno));
}

/* a child that drops from root to 65534 with RLIMIT_CORE open and dies of
 * SIGSEGV in dir: did it say "core dumped"? */
static int crash_dropped(const char *dir)
{
	pid_t c = fork();
	if (c == 0) {
		struct rlimit rl = { RLIM_INFINITY, RLIM_INFINITY };
		setrlimit(RLIMIT_CORE, &rl);
		if (chdir(dir) || setgid(65534) || setuid(65534)) _exit(9);
		signal(SIGSEGV, SIG_DFL);
		kill(getpid(), SIGSEGV);
		_exit(8);
	}
	int st = 0;
	waitpid(c, &st, 0);
	return WIFSIGNALED(st) && WCOREDUMP(st);
}

static void suid_dumpable(int touch)
{
	const char *K = "/proc/sys/fs/suid_dumpable";
	char was[32];
	int r = read_knob(K, was, sizeof was);
	CHECK(r == 0, "%s: %s", K, r ? strerror(-r) : "");
	if (r) return;
	CHECK(was[0] >= '0' && was[0] <= '2' && was[1] == '\n', "%s reads '%s', want 0, 1 or 2", K, was);
	struct stat sb;
	CHECK(stat(K, &sb) == 0 && sb.st_uid == 0 && (sb.st_mode & 0777) == 0644, "%s: uid %u mode %o, want 0 0644", K, sb.st_uid, sb.st_mode & 0777);
	if (!touch) return;
	CHECK(strcmp(was, "0\n") == 0, "%s is '%s' at boot, want 0", K, was);

	pid_t c = fork();
	if (c == 0) {
		if (setgid(65534) || setuid(65534)) _exit(9);
		_exit(write_knob(K, "1\n") == EACCES ? 0 : 1);
	}
	int st = 0;
	waitpid(c, &st, 0);
	CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "uid 65534 wrote %s (status 0x%x), want EACCES", K, st);
	CHECK(write_knob(K, "3\n") == EINVAL, "%s took 3, want EINVAL", K);
	CHECK(write_knob(K, "-1\n") == EINVAL, "%s took -1, want EINVAL", K);

	/* 1: a task that dropped root stays dumpable and open to its new user */
	int fdn, pub, dump;
	CHECK(write_knob(K, "1\n") == 0, "write 1 to %s", K);
	char now[32];
	CHECK(read_knob(K, now, sizeof now) == 0 && strcmp(now, "1\n") == 0, "%s reads '%s' after writing 1", K, now);
	pid_t t = start_target(65534, 0, &fdn, &pub, &dump);
	if (t > 0) {
		CHECK(dump == 1, "suid_dumpable 1: a task that dropped root is dumpable %d, want 1", dump);
		owner_is("suid_dumpable 1", t, "fd", 65534, 0500);
		look("suid_dumpable 1, same uid", t, fdn, pub, 1, 65534);
		stop(t);
	} else CHECK(0, "suid_dumpable 1: target did not start");

	/* 2: dumpable 2, still closed to its user, /proc/<pid> root's */
	CHECK(write_knob(K, "2\n") == 0, "write 2 to %s", K);
	t = start_target(65534, 0, &fdn, &pub, &dump);
	if (t > 0) {
		CHECK(dump == 2, "suid_dumpable 2: a task that dropped root is dumpable %d, want 2", dump);
		owner_is("suid_dumpable 2", t, "fd", 0, 0500);
		look("suid_dumpable 2, same uid", t, fdn, pub, 0, 65534);
		stop(t);
	} else CHECK(0, "suid_dumpable 2: target did not start");

	/* cores: none from a task that is not dumpable; under 2 only to an absolute
	 * pattern, and root's 0600 */
	const char *P = "/proc/sys/kernel/core_pattern";
	char pat[160];
	if (read_knob(P, pat, sizeof pat) == 0) {
		char *nl = strchr(pat, '\n');
		if (nl) *nl = 0;
		mkdir("/tmp/procpriv-cores", 01777);
		chmod("/tmp/procpriv-cores", 01777);
		unlink("/tmp/procpriv-cores/core");
		unlink("/tmp/procpriv-cores/abs");
		CHECK(write_knob(P, "core\n") == 0, "core_pattern core");
		int dumped = crash_dropped("/tmp/procpriv-cores");
		CHECK(!dumped && access("/tmp/procpriv-cores/core", F_OK) != 0, "suid_dumpable 2, relative core_pattern: a core was written (%d)", dumped);
		CHECK(write_knob(P, "/tmp/procpriv-cores/abs\n") == 0, "core_pattern absolute");
		dumped = crash_dropped("/tmp/procpriv-cores");
		CHECK(dumped, "suid_dumpable 2, absolute core_pattern: no core");
		CHECK(stat("/tmp/procpriv-cores/abs", &sb) == 0 && sb.st_uid == 0 && (sb.st_mode & 0777) == 0600, "suid_dumpable 2: the core is uid %u mode %o, want root's 0600", sb.st_uid, sb.st_mode & 0777);
		unlink("/tmp/procpriv-cores/abs");
		CHECK(write_knob(K, "0\n") == 0, "write 0 to %s", K);
		dumped = crash_dropped("/tmp/procpriv-cores");
		CHECK(!dumped && access("/tmp/procpriv-cores/abs", F_OK) != 0, "suid_dumpable 0: a task that dropped root left a core");
		unlink("/tmp/procpriv-cores/abs");
		rmdir("/tmp/procpriv-cores");
		strcat(pat, "\n");
		write_knob(P, pat);
	}

	write_knob(K, was);
}

int main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	self_exe = argv[0];
	if (argc == 5 && strcmp(argv[1], "--wait") == 0) {   /* the exec'd target */
		int m[3] = { atoi(argv[3]), prctl(PR_GET_DUMPABLE), atoi(argv[4]) };
		if (write(atoi(argv[2]), m, sizeof m) != sizeof m) return 11;
		close(atoi(argv[2]));
		pause();
		return 0;
	}
	if (argc == 2 && strcmp(argv[1], "--dumpable") == 0)
		return prctl(PR_GET_DUMPABLE);
	if (geteuid() != 0) { printf("needs root\nFAIL\n"); return 1; }
	int touch = !(argc > 1 && strcmp(argv[1], "nosysctl") == 0);
	int s = open(SECRET, O_RDWR | O_CREAT | O_TRUNC, 0600);
	CHECK(s >= 0 && write(s, "root", 4) == 4, "make " SECRET);
	if (s >= 0) close(s);
	s = open(PUBLIC, O_RDWR | O_CREAT | O_TRUNC, 0644);
	CHECK(s >= 0 && fchmod(s, 0644) == 0, "make " PUBLIC);
	if (s >= 0) close(s);
	char kn[32] = "0";
	read_knob("/proc/sys/fs/suid_dumpable", kn, sizeof kn);
	char ys[8] = "0";
	if (read_knob("/proc/sys/kernel/yama/ptrace_scope", ys, sizeof ys) == 0) yama = ys[0] != '0';
	guarded_proc(kn[0] - '0');
	suid_dumpable(touch);
	unlink(SECRET);
	unlink(PUBLIC);
	printf("%s\n", fails ? "FAIL" : "OK");
	return fails ? 1 : 0;
}
