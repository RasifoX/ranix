/* glibc posix_spawn + vfork + fork/exec smoke test, mirroring how dpkg-deb
 * spawns helpers (it posix_spawns a decompressor and pipes its output to tar).
 * Built with the stock glibc host toolchain like c_glibc.c. Exercises glibc's
 * clone(CLONE_VM|CLONE_VFORK) child path that surfaced a Ranix fault under
 * dpkg-deb. Prints "spawn-ok" only if every child ran and was reaped. */
#define _GNU_SOURCE
#include <unistd.h>
#include <spawn.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

extern char **environ;

static int run_spawn(void)
{
	pid_t pid;
	char *argv[] = { "/bin/true", NULL };
	if (posix_spawn(&pid, "/bin/true", NULL, NULL, argv, environ) != 0)
		return 1;
	int st;
	if (waitpid(pid, &st, 0) != pid) return 1;
	return WIFEXITED(st) && WEXITSTATUS(st) == 0 ? 0 : 1;
}

static int run_vfork(void)
{
	pid_t pid = vfork();
	if (pid == 0) {
		execl("/bin/true", "true", (char *)NULL);
		_exit(127);
	}
	if (pid < 0) return 1;
	int st;
	if (waitpid(pid, &st, 0) != pid) return 1;
	return WIFEXITED(st) && WEXITSTATUS(st) == 0 ? 0 : 1;
}

/* posix_spawn a child whose stdout is a pipe, read it back — the exact shape of
 * dpkg-deb's "spawn helper, capture its output" pattern. */
static int run_spawn_pipe(void)
{
	int fds[2];
	if (pipe(fds) != 0) return 1;
	posix_spawn_file_actions_t fa;
	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_adddup2(&fa, fds[1], 1);
	posix_spawn_file_actions_addclose(&fa, fds[0]);
	posix_spawn_file_actions_addclose(&fa, fds[1]);
	pid_t pid;
	char *argv[] = { "/bin/echo", "pipe-ok", NULL };
	int rc = posix_spawn(&pid, "/bin/echo", &fa, NULL, argv, environ);
	posix_spawn_file_actions_destroy(&fa);
	close(fds[1]);
	if (rc != 0) { close(fds[0]); return 1; }
	char buf[64];
	ssize_t n = read(fds[0], buf, sizeof(buf) - 1);
	close(fds[0]);
	int st;
	waitpid(pid, &st, 0);
	if (n <= 0) return 1;
	buf[n] = '\0';
	return strncmp(buf, "pipe-ok", 7) == 0 ? 0 : 1;
}

#include <pthread.h>

/* fork()+exec() issued from a worker thread (not the main thread): the child
 * runs on the worker's thread stack via COW, the exact shape dpkg-deb uses when
 * a worker forks to pipe data through tar. Exercises the fork-from-thread path. */
static void *thread_forker(void *arg)
{
	(void)arg;
	pid_t pid = fork();
	if (pid == 0) {
		execl("/bin/true", "true", (char *)NULL);
		_exit(127);
	}
	if (pid < 0) return (void *)1;
	int st;
	if (waitpid(pid, &st, 0) != pid) return (void *)1;
	return (void *)(long)(WIFEXITED(st) && WEXITSTATUS(st) == 0 ? 0 : 1);
}

static int run_thread_fork(void)
{
	pthread_t t;
	if (pthread_create(&t, NULL, thread_forker, NULL) != 0) return 1;
	void *r = NULL;
	pthread_join(t, &r);
	return r == NULL ? 0 : 1;
}

/* A worker that just blocks, so it is alive (its stack + TCB mapped) when the
 * main thread forks. glibc's fork in the child walks the thread/stack lists and
 * touches those dead siblings' descriptors — the exact multithreaded-fork shape
 * dpkg-deb hits (a blocked liblzma worker while the main thread forks tar). */
static pthread_mutex_t blk_m = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  blk_c = PTHREAD_COND_INITIALIZER;
static int blk_go;

static void *thread_blocker(void *arg)
{
	(void)arg;
	pthread_mutex_lock(&blk_m);
	while (!blk_go) pthread_cond_wait(&blk_c, &blk_m);
	pthread_mutex_unlock(&blk_m);
	return NULL;
}

static int run_fork_with_blocked_worker(void)
{
	pthread_t t;
	if (pthread_create(&t, NULL, thread_blocker, NULL) != 0) return 1;
	int rc = 1;
	pid_t pid = fork();                 /* main thread forks with worker blocked */
	if (pid == 0) {
		execl("/bin/true", "true", (char *)NULL);
		_exit(127);
	}
	if (pid > 0) {
		int st;
		if (waitpid(pid, &st, 0) == pid && WIFEXITED(st) && WEXITSTATUS(st) == 0) rc = 0;
	}
	pthread_mutex_lock(&blk_m); blk_go = 1; pthread_cond_signal(&blk_c); pthread_mutex_unlock(&blk_m);
	pthread_join(t, NULL);
	return rc;
}

int main(void)
{
	if (run_vfork() != 0)       return 2;
	if (run_spawn() != 0)       return 3;
	if (run_spawn_pipe() != 0)  return 4;
	if (run_thread_fork() != 0) return 5;
	if (run_fork_with_blocked_worker() != 0) return 6;
	return write(1, "spawn-ok\n", 9) == 9 ? 0 : 1;
}
