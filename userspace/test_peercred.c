/* SO_PEERCRED on AF_UNIX (kernel/sys/syscall_net.c): the peer's pid/uid/gid as of
 * connect() or socketpair(). fjord-sys uses it to tell a person at the desktop
 * from an agent over ssh. socketpair: both ends are us. listen/connect: the
 * server sees the client (here uid 1000), the client sees the server (root).
 * An unconnected socket gives pid 0, uid/gid -1, as on Linux. */
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <sys/stat.h>

#define FAIL(...) do { printf("FAIL " __VA_ARGS__); printf("\n"); return 1; } while (0)

static int cred(int fd, struct ucred *u)
{
	socklen_t l = sizeof *u;
	return getsockopt(fd, SOL_SOCKET, SO_PEERCRED, u, &l) < 0 ? -errno : (l == sizeof *u ? 0 : -999);
}

int main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	struct ucred u;
	int sv[2];
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv)) FAIL("socketpair");
	if (cred(sv[0], &u) || u.pid != getpid() || u.uid != geteuid()) FAIL("socketpair cred pid=%d uid=%d", u.pid, u.uid);
	int lone = socket(AF_UNIX, SOCK_STREAM, 0);
	if (cred(lone, &u) || u.pid != 0 || u.uid != (uid_t)-1) FAIL("unconnected: rc %d pid %d uid %d (Linux: 0, 0, -1)", cred(lone, &u), u.pid, (int)u.uid);
	char path[64]; snprintf(path, sizeof path, "/tmp/peercred.%d", (int)getpid());
	unlink(path);
	int ls = socket(AF_UNIX, SOCK_STREAM, 0);
	struct sockaddr_un a = { .sun_family = AF_UNIX };
	strcpy(a.sun_path, path);
	if (bind(ls, (void *)&a, sizeof a) || listen(ls, 1)) FAIL("bind/listen: %s", strerror(errno));
	chmod(path, 0777);
	int p[2]; pipe(p);
	pid_t c = fork();
	if (c == 0) {
		setgid(1000); setuid(1000);
		int s = socket(AF_UNIX, SOCK_STREAM, 0);
		if (connect(s, (void *)&a, sizeof a)) _exit(2);
		struct ucred su;
		if (cred(s, &su) || su.pid != getppid() || su.uid != 0) _exit(3);
		char x; read(p[0], &x, 1);
		_exit(0);
	}
	int as = accept(ls, NULL, NULL);
	if (as < 0) FAIL("accept");
	if (cred(as, &u)) FAIL("accepted cred: %d", cred(as, &u));
	int ok = u.pid == c && u.uid == 1000 && u.gid == 1000;
	write(p[1], "x", 1);
	int st; waitpid(c, &st, 0);
	unlink(path);
	if (!ok) FAIL("server sees pid=%d uid=%d gid=%d, want %d/1000/1000", u.pid, u.uid, u.gid, (int)c);
	if (!WIFEXITED(st) || WEXITSTATUS(st)) FAIL("client side check, exit %d", WEXITSTATUS(st));
	printf("OK\n");
	return 0;
}
