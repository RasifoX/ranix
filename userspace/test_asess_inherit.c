/* Every way of making a task keeps it in the session: fork, vfork, clone, clone3,
 * threads, posix_spawn, double fork with setsid. So does exec, of a setuid helper
 * or through ld.so. The task budget holds across all of them. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <spawn.h>
#include <stdint.h>
#include <sys/stat.h>
#include "asess_test.h"

#ifndef SYS_clone3
#define SYS_clone3 435
#endif

#define LDSO "/lib/ld-musl-x86_64.so.1"
#define DYNPROBE "/bin/test_asess_dynprobe"
#define SUID "/bin/test_suidhelper"

extern char **environ;

/* exit 0 only if this task is in session id and cannot leave it */
static int confined(unsigned long long id)
{
	return asess_self() == id && unshare(CLONE_NEWUSER) == -1 && errno == EPERM &&
	       geteuid() == UID;
}

static void wait_ok(pid_t p, const char *what)
{
	int st = 1;
	if (p < 0 || waitpid(p, &st, 0) != p || !WIFEXITED(st) || WEXITSTATUS(st)) {
		printf("FAIL %s: pid %d status %#x\n", what, (int)p, st);
		fails++;
	}
}

static void *thread_body(void *arg)
{
	unsigned long long id = *(unsigned long long *)arg;
	return (void *)(long)confined(id);
}

static void task_paths(unsigned long long id)
{
	pid_t p = fork();
	if (p == 0) _exit(!confined(id));
	wait_ok(p, "fork");

	p = vfork();
	if (p == 0) _exit(!confined(id));
	wait_ok(p, "vfork");

	p = (pid_t)syscall(SYS_clone, SIGCHLD, 0, 0, 0, 0);
	if (p == 0) _exit(!confined(id));
	wait_ok(p, "clone");

	struct { uint64_t flags, pidfd, child_tid, parent_tid, exit_signal, stack, stack_size, tls; } ca;
	memset(&ca, 0, sizeof(ca));
	ca.exit_signal = SIGCHLD;
	p = (pid_t)syscall(SYS_clone3, &ca, sizeof(ca));
	if (p == 0) _exit(!confined(id));
	wait_ok(p, "clone3");

	pthread_t t;
	void *res = NULL;
	CHECK(pthread_create(&t, NULL, thread_body, &id) == 0, "pthread_create");
	pthread_join(t, &res);
	CHECK(res == (void *)1L, "thread confined");

	/* a thread that forks */
	p = fork();
	if (p == 0) {
		if (pthread_create(&t, NULL, thread_body, &id)) _exit(2);
		pthread_join(t, &res);
		_exit(res != (void *)1L);
	}
	wait_ok(p, "thread in a child");

	char *argv[] = { "/proc/self/exe", "--probe", NULL };
	int psr = posix_spawn(&p, "/proc/self/exe", NULL, NULL, argv, environ);
	if (psr) { printf("FAIL posix_spawn: %d\n", psr); fails++; }
	else wait_ok(p, "posix_spawn then exec");

	/* double fork and setsid: an orphan is still a member */
	int w[2];
	CHECK(pipe(w) == 0, "pipe");
	p = fork();
	if (p == 0) {
		setsid();
		if (fork() != 0) _exit(0);
		sleep(1);   /* let the middle task die: reparented now */
		char ok = confined(id) && getppid() == 1;
		write(w[1], &ok, 1);
		_exit(0);
	}
	wait_ok(p, "first of a double fork");
	char ok = 0;
	close(w[1]);
	CHECK(read(w[0], &ok, 1) == 1 && ok == 1, "double fork orphan");
	close(w[0]);
}

static void capture(char *const argv[], char *out, size_t sz)
{
	int o[2];
	if (pipe(o)) { out[0] = 0; return; }
	pid_t p = fork();
	if (p == 0) {
		dup2(o[1], 1);
		close(o[0]); close(o[1]);
		execv(argv[0], argv);
		_exit(127);
	}
	close(o[1]);
	ssize_t n = read(o[0], out, sz - 1);
	close(o[0]);
	out[n > 0 ? n : 0] = '\0';
	waitpid(p, NULL, 0);
}

static void exec_paths(unsigned long long id)
{
	char out[128], want[128];

	char *suid[] = { SUID, NULL };
	capture(suid, out, sizeof(out));
	CHECK(strstr(out, "euid=1000") != NULL, "setuid helper keeps the caller's euid");

	if (access(LDSO, X_OK) == 0 && access(DYNPROBE, X_OK) == 0) {
		char *ld[] = { LDSO, DYNPROBE, NULL };
		capture(ld, out, sizeof(out));
		snprintf(want, sizeof(want), "id=%llu unshare=-1 errno=%d euid=1000", id, EPERM);
		if (!strstr(out, want)) {
			printf("FAIL ld.so re-exec: got '%s'\n", out);
			fails++;
		} else {
			puts("ld.so re-exec: confined");
		}
	} else {
		puts("SKIP ld.so re-exec: no loader or probe in the image");
	}
}

/* max_tasks counts threads too, and comes back as tasks exit */
static void budget(void)
{
	int keep[] = { 1 };
	long sfd = sess_create("/", UID, keep, 1, 4);
	CHECK(sfd >= 0, "create with a budget");
	long pid = sess_must_spawn(sfd);
	if (pid == 0) {
		int done[2];
		if (pipe(done)) _exit(9);
		pid_t held[8];
		int n = 0;
		for (;;) {
			pid_t p = fork();
			if (p == 0) { char c; close(done[1]); read(done[0], &c, 1); _exit(0); }
			if (p < 0) break;
			held[n++] = p;
			if (n == 8) break;
		}
		CHECK(n == 3 && errno == EAGAIN, "fork stops at max_tasks");
		close(done[1]);
		while (n) waitpid(held[--n], NULL, 0);
		pid_t p = fork();
		if (p == 0) _exit(0);
		CHECK(p > 0, "budget returns after exit");
		if (p > 0) waitpid(p, NULL, 0);
		_exit(fails ? 1 : 0);
	}
	sess_reap((pid_t)pid);
}

int main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	if (argc > 1 && strcmp(argv[1], "--probe") == 0)
		return !(asess_self() && unshare(CLONE_NEWUSER) == -1 && errno == EPERM);

	/* the helper must really be setuid root for the test to mean anything */
	struct stat st;
	CHECK(stat(SUID, &st) == 0, "stat the helper");
	if (!(st.st_mode & S_ISUID) || st.st_uid)
		CHECK(chown(SUID, 0, 0) == 0 && chmod(SUID, 04755) == 0, "make the helper setuid");

	int keep[] = { 1 };
	long sfd = sess_create("/", UID, keep, 1, 0);
	CHECK(sfd >= 0, "create");
	long pid = sess_must_spawn(sfd);
	if (pid == 0) {
		unsigned long long id = asess_self();
		task_paths(id);
		exec_paths(id);
		_exit(fails ? 1 : 0);
	}
	sess_reap((pid_t)pid);
	budget();
	return finish();
}
