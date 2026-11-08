/* SO_PASSCRED reads back as set (Chromium's crashpad handler checks it with
 * getsockopt and gave up on ENOPROTOOPT), and TCP keepalive timing options
 * are accepted (Chromium sets TCP_KEEPIDLE on every connection). */
#define _GNU_SOURCE
#include <stdio.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

int main(void)
{
	int sv[2], v = -1, one = 1;
	socklen_t l = sizeof v;
	if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) != 0 &&
	    socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) { printf("FAIL socketpair\n"); return 0; }
	if (getsockopt(sv[0], SOL_SOCKET, SO_PASSCRED, &v, &l) != 0 || v != 0) { printf("FAIL initial %d\n", v); return 0; }
	if (setsockopt(sv[0], SOL_SOCKET, SO_PASSCRED, &one, sizeof one) != 0) { printf("FAIL set\n"); return 0; }
	l = sizeof v;
	if (getsockopt(sv[0], SOL_SOCKET, SO_PASSCRED, &v, &l) != 0 || v != 1 || l != sizeof v) { printf("FAIL readback %d\n", v); return 0; }
	int t = socket(AF_INET, SOCK_STREAM, 0), sec = 45;
	if (t < 0 || setsockopt(t, IPPROTO_TCP, TCP_KEEPIDLE, &sec, sizeof sec) != 0 ||
	    setsockopt(t, IPPROTO_TCP, TCP_KEEPINTVL, &sec, sizeof sec) != 0) { printf("FAIL keepidle\n"); return 0; }
	printf("OK\n");
	return 0;
}
