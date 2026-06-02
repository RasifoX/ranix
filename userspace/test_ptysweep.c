/* pseudo-terminal sweep: open ptmx, find the pts number, open the slave, and
 * move data both ways (master<->slave). Exercises the Unix98 pty path that
 * ssh/tmux/script rely on. First failure or OK. */
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>

#ifndef TIOCGPTN
#define TIOCGPTN   0x80045430
#endif
#ifndef TIOCSPTLCK
#define TIOCSPTLCK 0x40045431
#endif

#define CHECK(c) do { if (!(c)) { printf("FAIL %d %s\n", __LINE__, #c); return 0; } } while (0)

int main(void)
{
	int m = open("/dev/ptmx", O_RDWR);
	CHECK(m >= 0);

	unsigned int ptn = 0;
	CHECK(ioctl(m, TIOCGPTN, &ptn) == 0);
	int unlock = 0;
	ioctl(m, TIOCSPTLCK, &unlock);   /* unlockpt (best-effort) */

	char path[32];
	snprintf(path, sizeof(path), "/dev/pts/%u", ptn);
	int s = open(path, O_RDWR);
	CHECK(s >= 0);

	/* master -> slave */
	CHECK(write(m, "hi", 2) == 2);
	char b[16];
	CHECK(read(s, b, 2) == 2 && memcmp(b, "hi", 2) == 0);

	/* slave -> master */
	CHECK(write(s, "yo", 2) == 2);
	int n = (int)read(m, b, sizeof(b));
	CHECK(n >= 2 && memchr(b, 'y', n) != NULL);   /* line discipline may add CR */

	close(s);
	close(m);
	puts("OK");
	return 0;
}
