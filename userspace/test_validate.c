/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* Arguments are checked, not waved through (LTP posix_fadvise02..04,
 * setsockopt01, getsockopt01, prctl02, prctl08): fadvise says EBADF, ESPIPE
 * or EINVAL; set/getsockopt refuse a bad level, name, length or pointer the
 * way Linux does and read back what was set; prctl keeps the timer slack and
 * refuses unprivileged securebits and bounding-set changes. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

static int bad;
static void want(const char *what, long r, int err)
{
	int got = r < 0 ? errno : 0;
	if (got != err) {
		printf("%s: want %s, got %s\n", what, err ? strerror(err) : "success", got ? strerror(got) : "success");
		bad++;
	}
}

/* as an unprivileged user: the errno prctl gives, or 0 */
static int unpriv_prctl(int opt, unsigned long a2)
{
	if (geteuid() != 0) return prctl(opt, a2, 0, 0, 0) < 0 ? errno : 0;
	pid_t k = fork();
	if (k == 0) {
		if (setgid(65534) || setuid(65534)) _exit(255);
		_exit(prctl(opt, a2, 0, 0, 0) < 0 ? errno : 0);
	}
	int st = 0;
	waitpid(k, &st, 0);
	return WIFEXITED(st) ? WEXITSTATUS(st) : 254;
}

int main(void)
{
	/* fadvise returns the error, it does not set errno */
	int fd = open("/tmp/validate", O_CREAT | O_RDWR | O_TRUNC, 0644), p[2];
	pipe(p);
	if (posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL) != 0) { puts("fadvise on a file failed"); bad++; }
	if (posix_fadvise(1000, 0, 0, POSIX_FADV_NORMAL) != EBADF) { puts("fadvise bad fd: not EBADF"); bad++; }
	if (posix_fadvise(p[0], 0, 0, POSIX_FADV_NORMAL) != ESPIPE) { puts("fadvise on a pipe: not ESPIPE"); bad++; }
	if (posix_fadvise(fd, 0, 0, 6) != EINVAL) { puts("fadvise advice 6: not EINVAL"); bad++; }
	if (posix_fadvise(fd, 0, -1, POSIX_FADV_NORMAL) != EINVAL) { puts("fadvise negative len: not EINVAL"); bad++; }

	int s = socket(AF_INET, SOCK_STREAM, 0), one = 1, v = 0;
	socklen_t l = sizeof(v);
	errno = 0; want("setsockopt NULL value", setsockopt(s, SOL_SOCKET, SO_OOBINLINE, NULL, sizeof(int)), EFAULT);
	errno = 0; want("setsockopt optlen 0", setsockopt(s, SOL_SOCKET, SO_OOBINLINE, &one, 0), EINVAL);
	errno = 0; want("setsockopt level 500", setsockopt(s, 500, SO_OOBINLINE, &one, sizeof one), ENOPROTOOPT);
	errno = 0; want("setsockopt UDP level on TCP", setsockopt(s, IPPROTO_UDP, SO_OOBINLINE, &one, sizeof one), ENOPROTOOPT);
	errno = 0; want("setsockopt IP -1", setsockopt(s, IPPROTO_IP, -1, &one, sizeof one), ENOPROTOOPT);
	errno = 0; want("setsockopt TCP -1", setsockopt(s, IPPROTO_TCP, -1, &one, sizeof one), ENOPROTOOPT);
	errno = 0; want("setsockopt on a file", setsockopt(fd, SOL_SOCKET, SO_OOBINLINE, &one, sizeof one), ENOTSOCK);
	errno = 0; want("setsockopt SO_OOBINLINE", setsockopt(s, SOL_SOCKET, SO_OOBINLINE, &one, sizeof one), 0);
	errno = 0; want("getsockopt SO_OOBINLINE", getsockopt(s, SOL_SOCKET, SO_OOBINLINE, &v, &l), 0);
	if (v != 1) { printf("SO_OOBINLINE reads back %d\n", v); bad++; }
	errno = 0; want("getsockopt level 500", getsockopt(s, 500, SO_OOBINLINE, &v, &l), EOPNOTSUPP);
	errno = 0; want("getsockopt UDP level on TCP", getsockopt(s, IPPROTO_UDP, SO_OOBINLINE, &v, &l), EOPNOTSUPP);
	errno = 0; want("getsockopt IP -1", getsockopt(s, IPPROTO_IP, -1, &v, &l), ENOPROTOOPT);
	errno = 0; want("getsockopt TCP -1", getsockopt(s, IPPROTO_TCP, -1, &v, &l), ENOPROTOOPT);
	l = (socklen_t)-1;
	errno = 0; want("getsockopt negative optlen", getsockopt(s, SOL_SOCKET, SO_OOBINLINE, &v, &l), EINVAL);
	errno = 0; want("getsockopt NULL optlen", getsockopt(s, SOL_SOCKET, SO_OOBINLINE, &v, NULL), EFAULT);

	long def = prctl(PR_GET_TIMERSLACK, 0, 0, 0, 0);
	errno = 0; want("PR_SET_TIMERSLACK 70000", prctl(PR_SET_TIMERSLACK, 70000, 0, 0, 0), 0);
	if (prctl(PR_GET_TIMERSLACK, 0, 0, 0, 0) != 70000) { puts("timer slack does not read back 70000"); bad++; }
	prctl(PR_SET_TIMERSLACK, 0, 0, 0, 0);
	if (prctl(PR_GET_TIMERSLACK, 0, 0, 0, 0) != def) { puts("PR_SET_TIMERSLACK 0 does not restore the default"); bad++; }
	if (unpriv_prctl(PR_SET_SECUREBITS, 0) != EPERM) { puts("unprivileged PR_SET_SECUREBITS: not EPERM"); bad++; }
	if (unpriv_prctl(PR_CAPBSET_DROP, 1) != EPERM) { puts("unprivileged PR_CAPBSET_DROP: not EPERM"); bad++; }
	if (prctl(PR_CAPBSET_READ, 0, 0, 0, 0) != 1) { puts("PR_CAPBSET_READ CAP_CHOWN: not 1"); bad++; }
	errno = 0; want("PR_CAPBSET_READ 1000", prctl(PR_CAPBSET_READ, 1000, 0, 0, 0), EINVAL);

	unlink("/tmp/validate");
	puts(bad ? "FAIL" : "OK");
	return bad != 0;
}
