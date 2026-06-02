/* TCP over IPv6 loopback (::1): a parent server (bind/listen/accept/echo) and a
 * forked client (connect/send/recv) exchange a message via ::1. Exercises the
 * full IPv6 TCP path — handshake, data, teardown — including the v6
 * pseudo-header checksum and the family-aware socket lookup. */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/wait.h>

#define PORT 12348

int main(void)
{
	int srv = socket(AF_INET6, SOCK_STREAM, 0);
	if (srv < 0) { puts("FAIL socket"); return 1; }
	int one = 1;
	setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

	struct sockaddr_in6 a;
	memset(&a, 0, sizeof(a));
	a.sin6_family = AF_INET6;
	a.sin6_port   = htons(PORT);
	a.sin6_addr   = in6addr_any;
	if (bind(srv, (struct sockaddr *)&a, sizeof(a)) < 0) { puts("FAIL bind"); return 1; }
	if (listen(srv, 4) < 0) { puts("FAIL listen"); return 1; }

	pid_t pid = fork();
	if (pid == 0) {
		usleep(100000);
		int c = socket(AF_INET6, SOCK_STREAM, 0);
		struct sockaddr_in6 d;
		memset(&d, 0, sizeof(d));
		d.sin6_family = AF_INET6;
		d.sin6_port   = htons(PORT);
		d.sin6_addr   = in6addr_loopback;
		if (connect(c, (struct sockaddr *)&d, sizeof(d)) < 0) _exit(1);
		if (write(c, "hello6", 6) != 6) _exit(2);
		char buf[16];
		int n = (int)read(c, buf, sizeof(buf));
		_exit(n == 6 && memcmp(buf, "hello6", 6) == 0 ? 0 : 3);
	}

	int cli = accept(srv, NULL, NULL);
	if (cli >= 0) {
		char buf[16];
		int n = (int)read(cli, buf, sizeof(buf));
		if (n > 0) write(cli, buf, (size_t)n);
		close(cli);
	}

	int st = 0;
	waitpid(pid, &st, 0);
	close(srv);
	puts(WIFEXITED(st) && WEXITSTATUS(st) == 0 ? "OK" : "FAIL");
	return 0;
}
