/* UDP over loopback: the parent binds and blocks in recvfrom while a forked
 * child sends a datagram to 127.0.0.1. Exercises UDP tx/rx through the kernel's
 * in-stack loopback. */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/wait.h>

#define PORT 12346

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
		sendto(c, "ping", 4, 0, (struct sockaddr *)&d, sizeof(d));
		_exit(0);
	}

	char buf[16];
	int n = (int)recvfrom(s, buf, sizeof(buf), 0, NULL, NULL);
	waitpid(pid, NULL, 0);
	close(s);
	puts(n == 4 && memcmp(buf, "ping", 4) == 0 ? "OK" : "FAIL");
	return 0;
}
