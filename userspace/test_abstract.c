/* abstract-namespace AF_UNIX sockets (sun_path[0]=='\0') — the binding D-Bus,
 * systemd and X11 use. A server binds an abstract name (no filesystem entry),
 * a client connects to it and they exchange a message. */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>

#define NAME "ranix.abstract.test"

static socklen_t fill(struct sockaddr_un *a)
{
	memset(a, 0, sizeof(*a));
	a->sun_family = AF_UNIX;
	a->sun_path[0] = '\0';                 /* abstract */
	memcpy(a->sun_path + 1, NAME, sizeof(NAME) - 1);
	return (socklen_t)(2 + 1 + (sizeof(NAME) - 1));   /* family + \0 + name */
}

int main(void)
{
	struct sockaddr_un a;
	socklen_t alen = fill(&a);

	int srv = socket(AF_UNIX, SOCK_STREAM, 0);
	if (srv < 0 || bind(srv, (struct sockaddr *)&a, alen) != 0) { printf("abstract-fail bind\n"); return 0; }
	if (listen(srv, 1) != 0) { printf("abstract-fail listen\n"); return 0; }

	pid_t pid = fork();
	if (pid == 0) {
		int cli = socket(AF_UNIX, SOCK_STREAM, 0);
		if (connect(cli, (struct sockaddr *)&a, alen) != 0) _exit(2);
		if (write(cli, "ping", 4) != 4) _exit(3);
		char b[8] = {0};
		if (read(cli, b, 4) != 4 || memcmp(b, "pong", 4) != 0) _exit(4);
		_exit(0);
	}

	int c = accept(srv, NULL, NULL);
	if (c < 0) { printf("abstract-fail accept\n"); return 0; }
	char b[8] = {0};
	if (read(c, b, 4) != 4 || memcmp(b, "ping", 4) != 0) { printf("abstract-fail read [%s]\n", b); return 0; }
	write(c, "pong", 4);

	int st;
	waitpid(pid, &st, 0);
	printf("%s\n", (WIFEXITED(st) && WEXITSTATUS(st) == 0) ? "OK" : "abstract-fail client");
	return 0;
}
