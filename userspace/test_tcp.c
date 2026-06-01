/* TCP over loopback: a parent server (bind/listen/accept/echo) and a forked
 * client (connect/send/recv) exchange a message via 127.0.0.1. Exercises the
 * full TCP path — three-way handshake, data, teardown — through the kernel's
 * in-stack loopback. */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/wait.h>

#define PORT 12345

int main(void)
{
	int srv = socket(AF_INET, SOCK_STREAM, 0);
	if (srv < 0) { puts("FAIL socket"); return 1; }
	int one = 1;
	setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

	struct sockaddr_in a;
	memset(&a, 0, sizeof(a));
	a.sin_family = AF_INET;
	a.sin_port   = htons(PORT);
	a.sin_addr.s_addr = htonl(INADDR_ANY);
	if (bind(srv, (struct sockaddr *)&a, sizeof(a)) < 0) { puts("FAIL bind"); return 1; }
	if (listen(srv, 4) < 0) { puts("FAIL listen"); return 1; }

	pid_t pid = fork();
	if (pid == 0) {
		usleep(100000);                 /* let the server reach accept() */
		int c = socket(AF_INET, SOCK_STREAM, 0);
		struct sockaddr_in d;
		memset(&d, 0, sizeof(d));
		d.sin_family = AF_INET;
		d.sin_port   = htons(PORT);
		d.sin_addr.s_addr = htonl(0x7f000001);   /* 127.0.0.1 */
		if (connect(c, (struct sockaddr *)&d, sizeof(d)) < 0) _exit(1);
		if (write(c, "hello", 5) != 5) _exit(2);
		char buf[16];
		int n = (int)read(c, buf, sizeof(buf));
		_exit(n == 5 && memcmp(buf, "hello", 5) == 0 ? 0 : 3);
	}

	int cli = accept(srv, NULL, NULL);
	if (cli >= 0) {
		char buf[16];
		int n = (int)read(cli, buf, sizeof(buf));
		if (n > 0) write(cli, buf, (size_t)n);    /* echo */
		close(cli);
	}

	int st = 0;
	waitpid(pid, &st, 0);
	close(srv);
	puts(WIFEXITED(st) && WEXITSTATUS(st) == 0 ? "OK" : "FAIL");
	return 0;
}
