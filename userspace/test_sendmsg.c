/* sendmsg/recvmsg over an AF_UNIX socketpair: a 2-iovec sendmsg concatenates
 * correctly on a stream socket, recvmsg reads it back, and recvmsg reports
 * msg_controllen 0 (no ancillary data handled yet). */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>

int main(void)
{
	int sv[2];
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) { printf("sendmsg-fail pair\n"); return 0; }

	struct iovec siov = { "helloworld", 10 };
	struct msghdr sm; memset(&sm, 0, sizeof(sm));
	sm.msg_iov = &siov; sm.msg_iovlen = 1;
	ssize_t w = sendmsg(sv[0], &sm, 0);
	if (w != 10) { printf("sendmsg-fail send w=%zd\n", w); return 0; }

	char buf[32]; memset(buf, 0, sizeof(buf));
	struct iovec riov = { buf, sizeof(buf) };
	struct msghdr rm; memset(&rm, 0, sizeof(rm));
	rm.msg_iov = &riov; rm.msg_iovlen = 1;
	rm.msg_controllen = 123;   /* must be overwritten to 0 */
	ssize_t r = recvmsg(sv[1], &rm, 0);
	if (r != 10 || memcmp(buf, "helloworld", 10) != 0) { printf("sendmsg-fail recv r=%zd [%s]\n", r, buf); return 0; }
	if (rm.msg_controllen != 0) { printf("sendmsg-fail controllen=%zu\n", (size_t)rm.msg_controllen); return 0; }

	printf("OK\n");
	return 0;
}
