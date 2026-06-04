/* IPv4 UDP fragmentation + reassembly over loopback: send a datagram larger
 * than the link MTU (so ip_tx splits it into several IP fragments) and verify
 * the receiver reassembles the exact bytes. */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/wait.h>

#define PORT 12355
#define DLEN 3000   /* > 1500 MTU → 3 fragments */

static void fill(unsigned char *b, int n) { for (int i = 0; i < n; i++) b[i] = (unsigned char)(i * 7 + 3); }

int main(void)
{
	int s = socket(AF_INET, SOCK_DGRAM, 0);
	if (s < 0) { puts("FAIL socket"); return 1; }

	struct sockaddr_in a;
	memset(&a, 0, sizeof(a));
	a.sin_family = AF_INET;
	a.sin_port   = htons(PORT);
	a.sin_addr.s_addr = htonl(INADDR_ANY);
	if (bind(s, (struct sockaddr *)&a, sizeof(a)) < 0) { puts("FAIL bind"); return 1; }

	pid_t pid = fork();
	if (pid == 0) {
		usleep(100000);
		int c = socket(AF_INET, SOCK_DGRAM, 0);
		struct sockaddr_in d;
		memset(&d, 0, sizeof(d));
		d.sin_family = AF_INET;
		d.sin_port   = htons(PORT);
		d.sin_addr.s_addr = htonl(0x7f000001);
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
