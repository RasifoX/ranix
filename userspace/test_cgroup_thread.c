/* Threads belong to their process's cgroup: pids.max counts them, memory charged
 * by a thread outlives it, cgroup.freeze stops them and a move takes them along. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>

#define MB (1024 * 1024)

static long cg_read(const char *cg, const char *file)
{
	char path[128], b[64] = "";
	snprintf(path, sizeof(path), "/cgroup/%s/%s", cg, file);
	int fd = open(path, O_RDONLY);
	if (fd < 0) return -1;
	read(fd, b, sizeof(b) - 1);
	close(fd);
	return atol(b);
}

static int cg_write(const char *cg, const char *file, long v)
{
	char path[128], b[32];
	snprintf(path, sizeof(path), "/cgroup/%s/%s", cg, file);
	int fd = open(path, O_WRONLY);
	if (fd < 0) return -1;
	int n = sprintf(b, "%ld\n", v);
	int r = write(fd, b, n) == n ? 0 : -1;
	close(fd);
	return r;
}

static int nprocs_lines(const char *cg)
{
	char path[128], b[256];
	snprintf(path, sizeof(path), "/cgroup/%s/cgroup.procs", cg);
	int fd = open(path, O_RDONLY);
	if (fd < 0) return -1;
	int n = read(fd, b, sizeof(b)), l = 0;
	for (int i = 0; i < n; i++) l += b[i] == '\n';
	close(fd);
	return l;
}

static void *sleeper(void *a)
{
	(void)a;
	pause();
	return NULL;
}

static void *toucher(void *a)
{
	char *m = a;
	for (int i = 0; i < MB; i += 4096) m[i] = 1;
	return NULL;
}

static volatile long *counter;

static void *spinner(void *a)
{
	(void)a;
	for (;;) (*counter)++;
	return NULL;
}

/* child: a process with a spinning thread and 1 MB it touched through a thread */
static void worker(int ready)
{
	pthread_t t;
	char *m = mmap(0, MB, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
	pthread_create(&t, NULL, toucher, m);
	pthread_join(t, NULL);
	pthread_create(&t, NULL, spinner, NULL);
	write(ready, "x", 1);
	pause();
	_exit(0);
}

#define FAIL(what) do { printf("FAIL %s\n", what); return 1; } while (0)

int main(void)
{
	mkdir("/cgroup", 0755);
	mount("none", "/cgroup", "cgroup2", 0, NULL);
	mkdir("/cgroup/tha", 0755);
	mkdir("/cgroup/thb", 0755);

	/* vfork is fork-backed; it must still work with proc_vfork gone */
	pid_t v = vfork();
	if (v == 0) _exit(7);
	int st;
	if (v < 0 || waitpid(v, &st, 0) != v || WEXITSTATUS(st) != 7) FAIL("vfork");
	printf("vfork ok\n");

	/* run the pids.max part in a process of its own so the limit dies with it */
	pid_t c = fork();
	if (c == 0) {
		pthread_t t[3];
		if (cg_write("tha", "pids.max", 3)) _exit(10);
		if (cg_write("tha", "cgroup.procs", getpid())) _exit(11);
		if (pthread_create(&t[0], NULL, sleeper, NULL)) _exit(12);
		if (pthread_create(&t[1], NULL, sleeper, NULL)) _exit(13);
		if (cg_read("tha", "pids.current") != 3) _exit(14);
		if (nprocs_lines("tha") != 1) _exit(15);
		if (pthread_create(&t[2], NULL, sleeper, NULL) != EAGAIN) _exit(16);
		/* a failed clone gives its slot back */
		if (cg_read("tha", "pids.current") != 3) _exit(17);
		/* and so does a failed fork, with every reference it took */
		int pp[2];
		char c2;
		if (pipe(pp)) _exit(18);
		for (int i = 0; i < 300; i++) {
			pid_t f = fork();
			if (f == 0) _exit(0);
			if (f != -1 || errno != EAGAIN) _exit(19);
			if (pthread_create(&t[2], NULL, sleeper, NULL) != EAGAIN) _exit(20);
		}
		if (cg_read("tha", "pids.current") != 3) _exit(21);
		if (write(pp[1], "y", 1) != 1 || read(pp[0], &c2, 1) != 1) _exit(22);
		close(pp[1]);
		if (read(pp[0], &c2, 1) != 0) _exit(23);   /* no copied write end survives */
		_exit(0);
	}
	waitpid(c, &st, 0);
	if (!WIFEXITED(st) || WEXITSTATUS(st)) { printf("FAIL pids rc=%d\n", WEXITSTATUS(st)); return 1; }
	if (cg_read("tha", "pids.current") != 0) FAIL("pids not returned on exit");
	printf("pids ok\n");
	cg_write("tha", "pids.max", 100);

	counter = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_SHARED, -1, 0);
	int p[2];
	pipe(p);
	c = fork();
	if (c == 0) {
		cg_write("tha", "cgroup.procs", getpid());
		worker(p[1]);
	}
	char x;
	read(p[0], &x, 1);

	long held = cg_read("tha", "memory.current");
	if (held < MB) { printf("FAIL thread memory lost after thread exit: %ld\n", held); kill(c, SIGKILL); return 1; }
	if (cg_read("tha", "pids.current") != 2) { kill(c, SIGKILL); FAIL("spinner not counted"); }
	printf("memory ok\n");

	cg_write("tha", "cgroup.freeze", 1);
	usleep(100000);
	long a = *counter;
	usleep(200000);
	long b = *counter;
	cg_write("tha", "cgroup.freeze", 0);
	if (a != b) { kill(c, SIGKILL); FAIL("frozen thread still running"); }
	for (int i = 0; i < 100 && *counter == b; i++) usleep(50000);   /* slow under emulation */
	if (*counter == b) { kill(c, SIGKILL); FAIL("thread did not thaw"); }
	printf("freeze ok\n");

	if (cg_write("thb", "cgroup.procs", c)) { kill(c, SIGKILL); FAIL("move"); }
	if (cg_read("tha", "pids.current") != 0 || cg_read("thb", "pids.current") != 2) {
		kill(c, SIGKILL);
		FAIL("thread left behind by move");
	}
	if (cg_read("tha", "memory.current") >= MB || cg_read("thb", "memory.current") < MB) {
		kill(c, SIGKILL);
		FAIL("charge did not move");
	}
	printf("move ok\n");

	kill(c, SIGKILL);
	waitpid(c, &st, 0);
	/* wait can return before a sibling thread finishes dying */
	for (int i = 0; i < 100 && (cg_read("thb", "pids.current") || cg_read("thb", "memory.current")); i++)
		usleep(10000);
	if (cg_read("thb", "pids.current") != 0) FAIL("thb pids after exit");
	if (cg_read("thb", "memory.current") != 0) FAIL("thb memory after exit");
	printf("exit ok\nOK\n");
	return 0;
}
