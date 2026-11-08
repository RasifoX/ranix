/* fcntl record locks are the process's, whichever thread took them: another
 * thread of the same process takes an overlapping lock freely, a thread that
 * exits leaves them in place, and another process still sees the conflict.
 * SQLite works from worker threads; per-thread ownership gave VS Code
 * "SQLITE_BUSY: database is locked" against itself. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>

static int fd;

static int lk(int f, int type)
{
	struct flock l = {0};
	l.l_type = type; l.l_whence = SEEK_SET; l.l_start = 0; l.l_len = 100;
	return fcntl(f, F_SETLK, &l);
}

static void *th(void *a)
{
	(void)a;
	return (void *)(long)lk(fd, F_WRLCK);
}

static int other_process_blocked(const char *path)
{
	pid_t c = fork();
	if (c == 0) {
		int f = open(path, O_RDWR);
		_exit(lk(f, F_WRLCK) == -1 ? 0 : 1);
	}
	int st;
	waitpid(c, &st, 0);
	return WIFEXITED(st) && WEXITSTATUS(st) == 0;
}

int main(void)
{
	char path[] = "/tmp/lockthreadXXXXXX";
	fd = mkstemp(path);
	if (fd < 0) { printf("FAIL mkstemp\n"); return 0; }
	if (lk(fd, F_WRLCK) != 0) { printf("FAIL main lock\n"); return 0; }
	pthread_t t;
	void *r;
	pthread_create(&t, NULL, th, NULL);
	pthread_join(t, &r);
	if ((long)r != 0) { printf("FAIL thread blocked by own process\n"); unlink(path); return 0; }
	if (!other_process_blocked(path)) { printf("FAIL other process got the lock after thread exit\n"); unlink(path); return 0; }
	unlink(path);
	printf("OK\n");
	return 0;
}
