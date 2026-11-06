/* Open file description locks (F_OFD_GETLK/SETLK/SETLKW). QEMU probes for them
 * with F_OFD_GETLK and then locks image bytes with them; when the kernel took the
 * commands as no-ops, F_OFD_GETLK left l_type as asked and QEMU refused every
 * disk ("Failed to get consistent read lock").
 * Checks: a free range reads back F_UNLCK; two opens in one process conflict;
 * the holder shows l_pid -1; a nonzero l_pid is EINVAL; closing a dup keeps the
 * lock; closing the last fd of the open file drops it. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#ifndef F_OFD_GETLK
#define F_OFD_GETLK  36
#define F_OFD_SETLK  37
#define F_OFD_SETLKW 38
#endif

static int lk(int fd, int cmd, short type, off_t start, off_t len, struct flock *out)
{
	struct flock fl;
	memset(&fl, 0, sizeof fl);
	fl.l_type = type; fl.l_whence = SEEK_SET; fl.l_start = start; fl.l_len = len;
	int r = fcntl(fd, cmd, &fl);
	if (out) *out = fl;
	return r < 0 ? -errno : 0;
}

#define FAIL(...) do { printf("FAIL " __VA_ARGS__); printf("\n"); return 1; } while (0)

int main(void)
{
	const char *p = "/tmp/ofdlock.dat";
	int a = open(p, O_RDWR | O_CREAT | O_TRUNC, 0600), b = open(p, O_RDWR);
	if (a < 0 || b < 0) FAIL("open");
	struct flock g;
	if (lk(a, F_OFD_GETLK, F_WRLCK, 100, 1, &g) || g.l_type != F_UNLCK) FAIL("free range type=%d", g.l_type);
	if (lk(a, F_OFD_SETLK, F_RDLCK, 100, 1, NULL)) FAIL("read lock a");
	if (lk(b, F_OFD_SETLK, F_RDLCK, 100, 1, NULL)) FAIL("shared read lock b");
	if (lk(b, F_OFD_SETLK, F_UNLCK, 100, 1, NULL)) FAIL("unlock b");
	if (lk(a, F_OFD_SETLK, F_WRLCK, 200, 4, NULL)) FAIL("write lock a");
	int r = lk(b, F_OFD_SETLK, F_WRLCK, 201, 1, NULL);
	if (r != -EAGAIN && r != -EACCES) FAIL("second open did not conflict r=%d", r);
	if (lk(b, F_OFD_GETLK, F_RDLCK, 200, 4, &g) || g.l_type != F_WRLCK || g.l_pid != -1 || g.l_start != 200)
		FAIL("getlk holder type=%d pid=%d start=%ld", g.l_type, (int)g.l_pid, (long)g.l_start);
	if (lk(a, F_OFD_GETLK, F_WRLCK, 200, 4, &g) || g.l_type != F_UNLCK) FAIL("own lock reported");
	struct flock bad; memset(&bad, 0, sizeof bad); bad.l_type = F_RDLCK; bad.l_pid = 1;
	if (fcntl(a, F_OFD_SETLK, &bad) != -1 || errno != EINVAL) FAIL("l_pid!=0 accepted");
	int d = dup(a);
	close(d);
	r = lk(b, F_OFD_SETLK, F_WRLCK, 200, 1, NULL);
	if (r != -EAGAIN && r != -EACCES) FAIL("closing a dup dropped the lock r=%d", r);
	close(a);
	if (lk(b, F_OFD_SETLKW, F_WRLCK, 200, 1, NULL)) FAIL("lock not dropped at last close");
	close(b); unlink(p);
	printf("OK\n");
	return 0;
}
