/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* USB serial port in the test VM: QEMU's usb-serial (an FTDI FT232) on the xHCI
 * bus, whose far end is one FIFO that QEMU both writes and reads, so every byte
 * sent comes back. Checks the node (0660 root:dialout, 188:0), termios in and
 * out (TCSETS and TCSETS2 with 250000), raw and canonical reads, VTIME, ONLCR,
 * ICRNL, DTR/RTS (open raises them, B0 and TIOCMBIC drop them), flush, poll,
 * a write longer than one transfer, who may open it, and TIOCEXCL.
 * Prints OK, or the first step that failed. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>

#define DEV "/dev/ttyUSB0"

struct t2 {                     /* struct termios2 */
	unsigned int iflag, oflag, cflag, lflag;
	unsigned char line, cc[19];
	unsigned int ispeed, ospeed;
};
#define T_TCGETS2 0x802C542Au
#define T_TCSETS2 0x402C542Bu
#define T_BOTHER  0x1000u
#define T_CBAUD   0x100Fu

static int fail(const char *step)
{
	printf("userial-fail %s errno=%d\n", step, errno);
	return 1;
}

static double now(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* Read exactly n bytes, or give up after secs. Returns how many came. */
static int read_n(int fd, char *buf, int n, double secs)
{
	int got = 0;
	double end = now() + secs;
	while (got < n && now() < end) {
		struct pollfd p = { fd, POLLIN, 0 };
		if (poll(&p, 1, 200) <= 0) continue;
		int r = (int)read(fd, buf + got, (size_t)(n - got));
		if (r > 0) got += r;
	}
	return got;
}

static int raw(int fd, speed_t sp, int vmin, int vtime)
{
	struct termios t;
	if (tcgetattr(fd, &t)) return -1;
	cfmakeraw(&t);
	cfsetspeed(&t, sp);
	t.c_cc[VMIN] = (cc_t)vmin;
	t.c_cc[VTIME] = (cc_t)vtime;
	return tcsetattr(fd, TCSANOW, &t);
}

static int wires(int fd)
{
	int m = 0;
	return ioctl(fd, TIOCMGET, &m) ? -1 : m & (TIOCM_DTR | TIOCM_RTS);
}

/* A child that opens the port as uid/gid 65534, in dialout (20) or not. 0 = opened. */
static int open_as_nobody(int dialout)
{
	pid_t pid = fork();
	if (pid == 0) {
		gid_t g = 20;
		if (setgroups(dialout ? 1 : 0, &g) || setgid(65534) || setuid(65534)) _exit(9);
		int fd = open(DEV, O_RDWR | O_NOCTTY);
		if (fd >= 0) { close(fd); _exit(0); }
		_exit(errno == EACCES ? 1 : errno == EBUSY ? 2 : 3);
	}
	int st = 0;
	waitpid(pid, &st, 0);
	return WIFEXITED(st) ? WEXITSTATUS(st) : 8;
}

static int t_node(void)
{
	struct stat st;
	if (stat(DEV, &st)) return fail("node");
	if (!S_ISCHR(st.st_mode) || (st.st_mode & 0777) != 0660 || st.st_uid || st.st_gid != 20)
		return printf("userial-fail mode %o uid %d gid %d\n", st.st_mode, st.st_uid, st.st_gid), 1;
	if (major(st.st_rdev) != 188 || minor(st.st_rdev) != 0)
		return printf("userial-fail rdev %u:%u\n", major(st.st_rdev), minor(st.st_rdev)), 1;
	if (open_as_nobody(0) != 1) return fail("nobody-opened");
	if (open_as_nobody(1) != 0) return fail("dialout-denied");
	return 0;
}

static int t_defaults(int fd)
{
	struct termios t;
	if (!isatty(fd) || tcgetattr(fd, &t)) return fail("isatty");
	if (cfgetospeed(&t) != B9600 || (t.c_cflag & CSIZE) != CS8 || !(t.c_cflag & CREAD) ||
	    !(t.c_cflag & HUPCL) || (t.c_lflag & (ICANON | ECHO | ISIG)) != (ICANON | ECHO | ISIG))
		return printf("userial-fail defaults c=%o l=%o\n", t.c_cflag, t.c_lflag), 1;
	if (wires(fd) != (TIOCM_DTR | TIOCM_RTS)) return fail("open-raises-dtr");
	return 0;
}

static int t_loop(int fd)
{
	char buf[4096];
	if (raw(fd, B115200, 0, 10)) return fail("raw");
	struct termios t;
	tcgetattr(fd, &t);
	if (cfgetospeed(&t) != B115200 || (t.c_lflag & (ICANON | ECHO | ISIG)) || (t.c_oflag & OPOST))
		return fail("raw-readback");   /* cfmakeraw leaves ECHOE/ECHOK/ECHOCTL, as on Linux */
	struct pollfd p = { fd, POLLOUT, 0 };
	if (poll(&p, 1, 0) != 1 || !(p.revents & POLLOUT)) return fail("pollout");
	const char *m = "ranix-serial-0123456789\n";
	if (write(fd, m, strlen(m)) != (ssize_t)strlen(m)) return fail("write");
	int n = read_n(fd, buf, (int)strlen(m), 3);
	if (n != (int)strlen(m) || memcmp(buf, m, strlen(m))) return printf("userial-fail echo %d\n", n), 1;
	double t0 = now();
	n = (int)read(fd, buf, 10);                /* VMIN 0, VTIME 1 s, nothing sent */
	double dt = now() - t0;
	if (n != 0 || dt < 0.5 || dt > 3) return printf("userial-fail vtime n=%d dt=%.2f\n", n, dt), 1;
	for (int i = 0; i < 3000; i++) buf[i] = (char)('a' + i % 23);
	if (write(fd, buf, 3000) != 3000) return fail("write-3000");
	char back[3000];
	n = read_n(fd, back, 3000, 5);
	if (n != 3000 || memcmp(buf, back, 3000)) return printf("userial-fail 3000 got %d\n", n), 1;
	return 0;
}

static int t_cooked(int fd)
{
	char buf[64];
	struct termios t;
	tcgetattr(fd, &t);
	t.c_oflag |= OPOST | ONLCR;
	tcsetattr(fd, TCSANOW, &t);
	if (write(fd, "a\nb", 3) != 3 || read_n(fd, buf, 4, 3) != 4 || memcmp(buf, "a\r\nb", 4))
		return fail("onlcr");
	t.c_oflag &= ~OPOST;
	t.c_iflag |= ICRNL;
	t.c_lflag |= ICANON;
	tcsetattr(fd, TCSANOW, &t);
	if (write(fd, "abc\rdef\n", 8) != 8) return fail("canon-write");
	int n = (int)read(fd, buf, sizeof buf);
	if (n != 4 || memcmp(buf, "abc\n", 4)) return printf("userial-fail canon1 %d\n", n), 1;
	n = (int)read(fd, buf, sizeof buf);
	if (n != 4 || memcmp(buf, "def\n", 4)) return printf("userial-fail canon2 %d\n", n), 1;
	return 0;
}

static int t_wires(int fd)
{
	int m = TIOCM_DTR;
	if (ioctl(fd, TIOCMBIC, &m) || wires(fd) != TIOCM_RTS) return fail("tiocmbic");
	if (ioctl(fd, TIOCMBIS, &m) || wires(fd) != (TIOCM_DTR | TIOCM_RTS)) return fail("tiocmbis");
	if (raw(fd, B0, 0, 10) || wires(fd) != 0) return fail("b0-drops");
	if (raw(fd, B115200, 0, 10) || wires(fd) != (TIOCM_DTR | TIOCM_RTS)) return fail("b0-back");
	return 0;
}

static int t_other(int fd)
{
	struct t2 t;
	char buf[16];
	if (ioctl(fd, T_TCGETS2, &t)) return fail("tcgets2");
	t.cflag = (t.cflag & ~T_CBAUD) | T_BOTHER;
	t.ispeed = t.ospeed = 250000;
	if (ioctl(fd, T_TCSETS2, &t)) return fail("tcsets2");
	memset(&t, 0, sizeof t);
	if (ioctl(fd, T_TCGETS2, &t) || t.ospeed != 250000 || (t.cflag & T_CBAUD) != T_BOTHER)
		return printf("userial-fail 250000 %u %x\n", t.ospeed, t.cflag), 1;
	if (write(fd, "x250k", 5) != 5 || read_n(fd, buf, 5, 3) != 5 || memcmp(buf, "x250k", 5))
		return fail("echo-250000");
	if (write(fd, "zzz", 3) != 3) return fail("flush-write");
	usleep(300000);
	int q = -1;
	if (ioctl(fd, FIONREAD, &q) || q != 3) return printf("userial-fail fionread %d\n", q), 1;
	if (tcflush(fd, TCIFLUSH) || ioctl(fd, FIONREAD, &q) || q != 0) return fail("tcflush");
	if (ioctl(fd, TIOCEXCL) || open_as_nobody(1) != 2) return fail("tiocexcl");
	if (ioctl(fd, TIOCNXCL) || open_as_nobody(1) != 0) return fail("tiocnxcl");
	if (tcdrain(fd)) return fail("tcdrain");
	return 0;
}

int main(void)
{
	if (t_node()) return 0;
	int fd = open(DEV, O_RDWR | O_NOCTTY);
	if (fd < 0) return fail("open"), 0;
	if (t_defaults(fd) || t_loop(fd) || t_cooked(fd) || t_wires(fd) || t_other(fd)) return 0;
	close(fd);
	printf("OK\n");
	return 0;
}
