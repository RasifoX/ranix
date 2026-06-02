/* UDP over IPv6 loopback (::1): the parent binds and blocks in recvfrom while a
 * forked child sends a datagram to ::1. Exercises the IPv6 UDP tx/rx path and
 * the mandatory IPv6 UDP pseudo-header checksum. */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/wait.h>

#define PORT 12347

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
		sendto(c, "ping6", 5, 0, (struct sockaddr *)&d, sizeof(d));
		_exit(0);
	}

	char buf[16];
	int n = (int)recvfrom(s, buf, sizeof(buf), 0, NULL, NULL);
	waitpid(pid, NULL, 0);
	close(s);
	puts(n == 5 && memcmp(buf, "ping6", 5) == 0 ? "OK" : "FAIL");
	return 0;
}
