/* IPv6 UDP fragmentation + reassembly over loopback (::1): send a datagram
 * larger than the link MTU so ipv6_tx emits Fragment-header'd packets, and
 * verify the receiver reassembles the exact bytes. */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/wait.h>

#define PORT 12356
#define DLEN 3000

static void fill(unsigned char *b, int n) { for (int i = 0; i < n; i++) b[i] = (unsigned char)(i * 5 + 11); }

int main(void)
{
	int s = socket(AF_INET6, SOCK_DGRAM, 0);
	if (s < 0) { puts("FAIL socket"); return 1; }

	struct sockaddr_in6 a;
	memset(&a, 0, sizeof(a));
	a.sin6_family = AF_INET6;
	a.sin6_port   = htons(PORT);
	a.sin6_addr   = in6addr_loopback;
	if (bind(s, (struct sockaddr *)&a, sizeof(a)) < 0) { puts("FAIL bind"); return 1; }

	pid_t pid = fork();
	if (pid == 0) {
		usleep(100000);
		int c = socket(AF_INET6, SOCK_DGRAM, 0);
		struct sockaddr_in6 d;
		memset(&d, 0, sizeof(d));
		d.sin6_family = AF_INET6;
		d.sin6_port   = htons(PORT);
		d.sin6_addr   = in6addr_loopback;
		unsigned char out[DLEN];
		fill(out, DLEN);
		sendto(c, out, DLEN, 0, (struct sockaddr *)&d, sizeof(d));
		_exit(0);
	}

	unsigned char buf[DLEN + 64];
	int n = (int)recvfrom(s, buf, sizeof(buf), 0, NULL, NULL);
	waitpid(pid, NULL, 0);
	close(s);

	unsigned char want[DLEN];
	fill(want, DLEN);
	puts(n == DLEN && memcmp(buf, want, DLEN) == 0 ? "OK" : "FAIL");
	return 0;
}
