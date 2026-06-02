/* socketpair(AF_UNIX) sweep: bidirectional stream transfer, EOF on peer close,
 * and SOCK_DGRAM message delivery. First failure or OK. */
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <sys/socket.h>

#define CHECK(c) do { if (!(c)) { printf("FAIL %d %s\n", __LINE__, #c); return 0; } } while (0)

int main(void)
{
	int sv[2];
	CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
	char b[16];
	CHECK(write(sv[0], "ping", 4) == 4);
	CHECK(read(sv[1], b, 4) == 4 && memcmp(b, "ping", 4) == 0);

	/* sv[1] -> sv[0] (bidirectional) */
	CHECK(write(sv[1], "pong", 4) == 4);
	CHECK(read(sv[0], b, 4) == 4 && memcmp(b, "pong", 4) == 0);

	/* closing one end yields EOF on the other */
	close(sv[1]);
	CHECK(read(sv[0], b, 4) == 0);
	close(sv[0]);

	/* datagram socketpair delivers a message (via write/read) */
	int dv[2];
	if (socketpair(AF_UNIX, SOCK_DGRAM, 0, dv) == 0) {
		CHECK(write(dv[0], "msg", 3) == 3);
		CHECK(read(dv[1], b, sizeof(b)) == 3 && memcmp(b, "msg", 3) == 0);
		close(dv[0]); close(dv[1]);
	}

	puts("OK");
	return 0;
}
