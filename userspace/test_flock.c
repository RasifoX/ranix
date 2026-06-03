/* flock(2): BSD advisory whole-file locks. Two independent open files contend;
 * exclusive locks conflict, shared locks coexist, LOCK_NB reports EWOULDBLOCK,
 * and a lock is released when its open file is closed. (dpkg/sqlite/daemons.) */
#define _GNU_SOURCE
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/file.h>

int main(void)
{
	int a = open("/tmp/lockf", O_CREAT | O_RDWR, 0644);
	int b = open("/tmp/lockf", O_RDWR);
	int c = open("/tmp/lockf", O_RDWR);
	if (a < 0 || b < 0 || c < 0) { printf("flock-fail open\n"); return 0; }

	/* exclusive: a holds, b is blocked */
	if (flock(a, LOCK_EX) != 0) { printf("flock-fail ex\n"); return 0; }
	errno = 0;
	if (flock(b, LOCK_EX | LOCK_NB) != -1 || errno != EWOULDBLOCK) { printf("flock-fail nb errno=%d\n", errno); return 0; }

	/* a releases → b can take it */
	if (flock(a, LOCK_UN) != 0) { printf("flock-fail un\n"); return 0; }
	if (flock(b, LOCK_EX | LOCK_NB) != 0) { printf("flock-fail reacquire\n"); return 0; }
	flock(b, LOCK_UN);

	/* shared: a and b coexist, an exclusive request conflicts */
	if (flock(a, LOCK_SH) != 0 || flock(b, LOCK_SH) != 0) { printf("flock-fail sh\n"); return 0; }
	errno = 0;
	if (flock(c, LOCK_EX | LOCK_NB) != -1 || errno != EWOULDBLOCK) { printf("flock-fail sh-vs-ex\n"); return 0; }

	/* closing the shared holders releases the lock → exclusive now succeeds */
	close(a); close(b);
	if (flock(c, LOCK_EX | LOCK_NB) != 0) { printf("flock-fail release-on-close\n"); return 0; }
	close(c);

	printf("OK\n");
	return 0;
}
