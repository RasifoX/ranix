/* A thread blocked in F_OFD_SETLKW while a sibling closes that fd: the
 * lock it then takes must go away with the file, not outlive it. Lock
 * ranges that overflow off_t are EOVERFLOW. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

#ifndef F_OFD_GETLK
#define F_OFD_GETLK 36
#define F_OFD_SETLK 37
#define F_OFD_SETLKW 38
#endif
static const char *path = "/tmp/ofdlockwait";
static int fda;
static volatile int done = -1;

static void *waiter(void *x)
{
	(void)x;
	struct flock l = { .l_type = F_WRLCK, .l_whence = SEEK_SET };
	done = fcntl(fda, F_OFD_SETLKW, &l) == 0 ? 0 : errno;
	return NULL;
}

int main(void)
{
	int fails = 0;
	int fd1 = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
	fda = open(path, O_RDWR);
	struct flock l = { .l_type = F_WRLCK, .l_whence = SEEK_SET };
	if (fd1 < 0 || fda < 0 || fcntl(fd1, F_OFD_SETLK, &l)) { printf("FAIL setup errno=%d\n", errno); return 1; }
	pthread_t t;
	pthread_create(&t, NULL, waiter, NULL);
	usleep(300000);
	close(fda);                         /* the waiter's fd goes while it sleeps */
	usleep(100000);
	l.l_type = F_UNLCK;
	fcntl(fd1, F_OFD_SETLK, &l);
	for (int i = 0; i < 300 && done < 0; i++) usleep(10000);
	if (done < 0) { printf("FAIL waiter never returned\n"); return 1; }
	pthread_join(t, NULL);
	usleep(50000);
	int fd3 = open(path, O_RDWR);
	struct flock q = { .l_type = F_WRLCK, .l_whence = SEEK_SET };
	if (fcntl(fd3, F_OFD_GETLK, &q)) { printf("FAIL getlk errno=%d\n", errno); fails++; }
	else if (q.l_type != F_UNLCK) { printf("FAIL lock of a closed file still held type=%d\n", q.l_type); fails++; }

	/* off_t overflow */
	struct flock o = { .l_type = F_RDLCK, .l_whence = SEEK_SET, .l_start = INT64_MAX - 15, .l_len = 32 };
	errno = 0;
	if (fcntl(fd3, F_OFD_SETLK, &o) != -1 || errno != EOVERFLOW) { printf("FAIL start+len overflow errno=%d\n", errno); fails++; }
	lseek(fd3, 100, SEEK_SET);
	struct flock c = { .l_type = F_RDLCK, .l_whence = SEEK_CUR, .l_start = INT64_MAX - 10, .l_len = 1 };
	errno = 0;
	if (fcntl(fd3, F_OFD_SETLK, &c) != -1 || errno != EOVERFLOW) { printf("FAIL SEEK_CUR overflow errno=%d\n", errno); fails++; }
	struct flock ok = { .l_type = F_RDLCK, .l_whence = SEEK_SET, .l_start = INT64_MAX - 15, .l_len = 16 };
	if (fcntl(fd3, F_OFD_SETLK, &ok) != 0) { printf("FAIL lock ending at OFF_MAX errno=%d\n", errno); fails++; }
	close(fd3); close(fd1); unlink(path);
	if (!fails) puts("OK");
	return fails ? 1 : 0;
}
