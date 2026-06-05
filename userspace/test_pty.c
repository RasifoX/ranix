/* pseudo-terminal: open /dev/ptmx, get the slave number via TIOCGPTN, open
 * /dev/pts/N, and check bytes pass both ways (master->slave, slave->master). */
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <termios.h>
#include <sys/ioctl.h>

#ifndef TIOCGPTN
#define TIOCGPTN  0x80045430
#endif
#ifndef TIOCSPTLCK
#define TIOCSPTLCK 0x40045431
#endif

int main(void)
{
	int m = open("/dev/ptmx", O_RDWR);
	if (m < 0) { puts("FAIL ptmx"); return 0; }

	int n = -1;
	if (ioctl(m, TIOCGPTN, &n) != 0 || n < 0) { puts("FAIL ptn"); return 0; }
	int zero = 0;
	ioctl(m, TIOCSPTLCK, &zero);

	char path[32];
	snprintf(path, sizeof(path), "/dev/pts/%d", n);
	int s = open(path, O_RDWR);
	if (s < 0) { puts("FAIL pts-open"); return 0; }

	/* raw byte-stream pty (no canonical buffering / echo), as openpty users do */
	struct termios tio;
	if (tcgetattr(s, &tio) == 0) { cfmakeraw(&tio); tcsetattr(s, TCSANOW, &tio); }

	char buf[16];
	if (write(m, "hi", 2) != 2) { puts("FAIL m-write"); return 0; }
	if (read(s, buf, sizeof(buf)) != 2 || memcmp(buf, "hi", 2) != 0) { puts("FAIL s-read"); return 0; }

	if (write(s, "yo!", 3) != 3) { puts("FAIL s-write"); return 0; }
	if (read(m, buf, sizeof(buf)) != 3 || memcmp(buf, "yo!", 3) != 0) { puts("FAIL m-read"); return 0; }

	close(s); close(m);
	puts("OK");
	return 0;
}
