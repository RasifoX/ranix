/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* sendfile/splice copy through a kernel buffer, so every driver's read and write
 * must accept one. `echo hi | cat` printed nothing when the console refused it:
 * busybox cat tries sendfile first and the bytes were gone before it fell back. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <pty.h>
#include <sys/eventfd.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include "asess_test.h"

#define F "/tmp/kbuf.dat"

static const char msg[] = "kernel-buffer\n";

static int input(void)
{
	int fd = open(F, O_CREAT | O_TRUNC | O_RDWR, 0644);
	CHECK(fd >= 0 && write(fd, msg, sizeof(msg) - 1) == sizeof(msg) - 1, "input file");
	lseek(fd, 0, SEEK_SET);
	return fd;
}

static void to_stream(const char *what, int out, int peer)
{
	char b[64] = {0};
	int in = input();
	long n = sendfile(out, in, NULL, sizeof(msg) - 1);
	if (n != sizeof(msg) - 1) { printf("FAIL sendfile to %s: %ld errno %d\n", what, n, errno); fails++; }
	else if (peer >= 0 && (read(peer, b, sizeof(b)) < n || memcmp(b, msg, (size_t)n - 1))) {
		printf("FAIL %s: bytes did not arrive\n", what);
		fails++;
	}
	close(in);
}

static void to_console(void)
{
	int c = open("/dev/console", O_WRONLY);
	CHECK(c >= 0, "open console");
	to_stream("console", c, -1);
	close(c);
}

static void to_pty_and_socket(void)
{
	int m, s, sv[2];
	CHECK(openpty(&m, &s, NULL, NULL, NULL) == 0, "openpty");
	to_stream("pty", s, m);
	close(m); close(s);
	CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "socketpair");
	to_stream("stream socket", sv[0], sv[1]);
	close(sv[0]); close(sv[1]);
	CHECK(socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) == 0, "socketpair");
	to_stream("dgram socket", sv[0], sv[1]);
	close(sv[0]); close(sv[1]);
}

static void from_zero(void)
{
	static const char *dev[] = { "/dev/zero", "/dev/urandom" };
	for (int i = 0; i < 2; i++) {
		int in = open(dev[i], O_RDONLY);
		int out = open(F, O_CREAT | O_TRUNC | O_WRONLY, 0644);
		long n = sendfile(out, in, NULL, 100);
		if (n != 100) { printf("FAIL sendfile from %s: %ld errno %d\n", dev[i], n, errno); fails++; }
		close(in); close(out);
	}
}

static void from_eventfd(void)
{
	int in = input(), out = eventfd(0, 0);
	unsigned long long v = 7, got = 0;
	CHECK(pwrite(in, &v, 8, 0) == 8, "eventfd payload");
	lseek(in, 0, SEEK_SET);
	long n = sendfile(out, in, NULL, 8);
	CHECK(n == 8, "sendfile to eventfd");
	CHECK(read(out, &got, 8) == 8 && got == 7, "eventfd value");
	close(in); close(out);
}

/* Linux refuses these up front; consuming them first loses data on any error. */
static void pipe_in_refused(void)
{
	int p[2], out = open("/dev/null", O_WRONLY);
	char b[4] = {0};
	CHECK(pipe(p) == 0, "pipe");
	CHECK(write(p[1], "abc", 3) == 3, "pipe write");
	DENIED(sendfile(out, p[0], NULL, 3), EINVAL, "sendfile from a pipe");
	CHECK(read(p[0], b, 3) == 3 && !memcmp(b, "abc", 3), "pipe bytes kept");
	close(p[0]); close(p[1]); close(out);
}

static void short_write_keeps_position(void)
{
	static char big[256 * 1024], back[256 * 1024];
	int p[2], in = open(F, O_CREAT | O_TRUNC | O_RDWR, 0644);
	for (size_t i = 0; i < sizeof(big); i++) big[i] = (char)(i * 31);
	CHECK(write(in, big, sizeof(big)) == sizeof(big), "fill");
	lseek(in, 0, SEEK_SET);
	CHECK(pipe2(p, O_NONBLOCK) == 0, "pipe2");
	long n = sendfile(p[1], in, NULL, sizeof(big));
	CHECK(n > 0 && n < (long)sizeof(big), "partial sendfile");
	CHECK(lseek(in, 0, SEEK_CUR) == n, "file position follows what was sent");
	long r = read(p[0], back, sizeof(back));
	CHECK(r == n && !memcmp(back, big, (size_t)n), "pipe holds the same bytes");
	close(in); close(p[0]); close(p[1]);
}

static void splice_to_pty(void)
{
	int m, s, p[2];
	char b[64] = {0};
	CHECK(openpty(&m, &s, NULL, NULL, NULL) == 0 && pipe(p) == 0, "setup");
	CHECK(write(p[1], "spl\n", 4) == 4, "pipe write");
	close(p[1]);
	CHECK(splice(p[0], NULL, s, NULL, 4, 0) == 4, "splice to pty");
	CHECK(read(m, b, sizeof(b)) >= 3 && !memcmp(b, "spl", 3), "splice bytes");
	close(p[0]); close(m); close(s);
}

/* the kernel-buffer path must stay closed to pointers from user space */
static void kernel_pointer_refused(void)
{
	int sv[2];
	char dummy[8];
	struct iovec v = { (void *)0xffffffff80100000UL, 64 };
	struct msghdr m = { .msg_iov = &v, .msg_iovlen = 1 };
	CHECK(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sv) == 0, "socketpair");
	DENIED(sendmsg(sv[0], &m, 0), EFAULT, "sendmsg of a kernel address");
	DENIED(read(sv[1], dummy, sizeof(dummy)), EAGAIN, "nothing leaked to the peer");
	close(sv[0]);
	close(sv[1]);
}

/* a pty copies under its lock: a bad pointer has to be refused before that */
static void pty_bad_pointer(void)
{
	int m, s;
	char *bad = (char *)0x1000;
	CHECK(openpty(&m, &s, NULL, NULL, NULL) == 0, "openpty");
	DENIED(write(s, bad, 4), EFAULT, "slave write from a bad pointer");
	DENIED(write(m, bad, 4), EFAULT, "master write from a bad pointer");
	CHECK(write(s, "x\n", 2) == 2, "slave write");
	DENIED(read(m, bad, 4), EFAULT, "master read to a bad pointer");
	CHECK(write(m, "y\n", 2) == 2, "master write");
	DENIED(read(s, bad, 4), EFAULT, "slave read to a bad pointer");
	close(m);
	close(s);
}

/* the reported symptom, end to end, through the real busybox pipeline */
static void shell_pipeline(void)
{
	int m, s;
	char b[64];
	size_t have = 0;
	CHECK(openpty(&m, &s, NULL, NULL, NULL) == 0, "openpty");
	pid_t pid = fork();
	if (pid == 0) {
		dup2(s, 1);
		close(m); close(s);
		execl("/bin/sh", "sh", "-c", "echo hi | cat; echo end", NULL);
		_exit(127);
	}
	close(s);
	for (;;) {
		ssize_t r = read(m, b + have, sizeof(b) - 1 - have);
		if (r <= 0) break;
		have += (size_t)r;
		b[have] = 0;
		if (strstr(b, "end")) break;
	}
	b[have] = 0;
	waitpid(pid, NULL, 0);
	if (!strstr(b, "hi")) { printf("FAIL echo hi | cat printed '%s'\n", b); fails++; }
	close(m);
}

int main(void)
{
	alarm(60);
	to_console();
	to_pty_and_socket();
	from_zero();
	from_eventfd();
	pipe_in_refused();
	short_write_keeps_position();
	splice_to_pty();
	kernel_pointer_refused();
	pty_bad_pointer();
	shell_pipeline();
	unlink(F);
	return finish();
}
