/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* /dev/input/mice: the PS/2 aux port (mouse or touchpad) end to end.
 * The test VM has QEMU's PS/2 mouse. Bytes written by root go to the i8042
 * "write aux output buffer" command and come back through IRQ12, so this
 * covers the interrupt line, the status-register routing between keyboard
 * and aux bytes, packet framing and the device file.
 * Checks: the device exists; a short read is EINVAL; with nothing queued a
 * non-blocking read is EAGAIN and poll times out; a packet written comes back
 * whole; a byte without bit 3 before a packet is dropped, not glued on; a
 * non-root opener gets EACCES. */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define DEV "/dev/input/mice"

static int fail(const char *m, int e)
{
	printf("FAIL %s errno=%d\n", m, e);
	return 1;
}

static int get3(int fd, unsigned char *b)
{
	struct pollfd p = {fd, POLLIN, 0};
	if (poll(&p, 1, 2000) != 1) return -1;
	return (int)read(fd, b, 3);
}

int main(void)
{
	unsigned char b[8];
	int fd = open(DEV, O_RDWR | O_NONBLOCK);
	if (fd < 0) return fail("open", errno);
	for (int i = 0; i < 100 && read(fd, b, 3) == 3; i++)
		;                               /* anything the device sent at boot */
	if (read(fd, b, 2) != -1 || errno != EINVAL) return fail("short read", errno);
	errno = 0;
	if (read(fd, b, 3) != -1 || errno != EAGAIN) return fail("empty read", errno);
	struct pollfd p = {fd, POLLIN, 0};
	if (poll(&p, 1, 50) != 0) return fail("empty poll", 0);

	static const unsigned char pk[3] = {0x29, 0x05, 0xFD};   /* left, dx 5, dy -3 */
	if (write(fd, pk, 3) != 3) return fail("write", errno);
	memset(b, 0, sizeof b);
	if (get3(fd, b) != 3 || memcmp(b, pk, 3)) {
		printf("FAIL packet %02x %02x %02x\n", b[0], b[1], b[2]);
		return 1;
	}
	static const unsigned char junk[4] = {0x00, 0x0A, 0x01, 0x02};  /* desync, then right, 1, 2 */
	if (write(fd, junk, 4) != 4) return fail("write junk", errno);
	memset(b, 0, sizeof b);
	if (get3(fd, b) != 3 || memcmp(b, junk + 1, 3)) {
		printf("FAIL resync %02x %02x %02x\n", b[0], b[1], b[2]);
		return 1;
	}
	if (read(fd, b, 3) != -1 || errno != EAGAIN) return fail("extra bytes", errno);

	pid_t c = fork();
	if (c == 0) {
		if (setgid(65534) || setuid(65534)) _exit(2);
		int f = open(DEV, O_RDONLY | O_NONBLOCK);
		_exit(f < 0 && errno == EACCES ? 0 : 3);
	}
	int st = 0;
	waitpid(c, &st, 0);
	if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) return fail("non-root open", WEXITSTATUS(st));
	close(fd);
	printf("OK\n");
	return 0;
}
