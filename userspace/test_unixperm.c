/* AF_UNIX path sockets: connect() needs write permission on the socket
 * file, and a relative name under a very long cwd must not overrun the
 * kernel's path buffer. */
#define _GNU_SOURCE
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

static int fails;

static int try_connect(const char *path)
{
	int s = socket(AF_UNIX, SOCK_STREAM, 0);
	struct sockaddr_un a = { .sun_family = AF_UNIX };
	strncpy(a.sun_path, path, sizeof a.sun_path - 1);
	int r = connect(s, (struct sockaddr *)&a, sizeof a);
	int e = errno;
	close(s);
	return r == 0 ? 0 : e;
}

static void perm(void)
{
	const char *path = "/tmp/unixperm.sock";
	unlink(path);
	int l = socket(AF_UNIX, SOCK_STREAM, 0);
	struct sockaddr_un a = { .sun_family = AF_UNIX };
	strcpy(a.sun_path, path);
	if (bind(l, (struct sockaddr *)&a, sizeof a) || listen(l, 8)) { printf("FAIL bind errno=%d\n", errno); fails++; return; }
	chmod(path, 0600);
	struct stat st = {0};
	if (stat(path, &st) || !S_ISSOCK(st.st_mode) || (st.st_mode & 0777) != 0600) { printf("FAIL mode=%o\n", st.st_mode); fails++; }
	pid_t p = fork();
	if (p == 0) {
		if (setgid(1000) || setuid(1000)) _exit(10);
		_exit(try_connect(path) == EACCES ? 0 : 11);
	}
	int w = 0;
	waitpid(p, &w, 0);
	if (!WIFEXITED(w) || WEXITSTATUS(w)) { printf("FAIL unprivileged connect to 0600 socket allowed\n"); fails++; }
	chmod(path, 0666);
	p = fork();
	if (p == 0) {
		if (setgid(1000) || setuid(1000)) _exit(10);
		_exit(try_connect(path) == 0 ? 0 : 12);
	}
	waitpid(p, &w, 0);
	if (!WIFEXITED(w) || WEXITSTATUS(w)) { printf("FAIL connect to 0666 socket refused step=%d\n", WEXITSTATUS(w)); fails++; }
	if (try_connect(path) != 0) { printf("FAIL root connect\n"); fails++; }
	close(l);
	unlink(path);
	/* an unprivileged bind leaves a socket file too (tmpfs and ramfs) */
	mkdir("/tmp/unixperm.d", 01777);
	chmod("/tmp/unixperm.d", 01777);
	p = fork();
	if (p == 0) {
		if (setgid(1000) || setuid(1000)) _exit(10);
		const char *q = "/tmp/unixperm.d/user.sock";
		int s = socket(AF_UNIX, SOCK_STREAM, 0);
		struct sockaddr_un b = { .sun_family = AF_UNIX };
		strcpy(b.sun_path, q);
		if (bind(s, (struct sockaddr *)&b, sizeof b)) _exit(13);
		struct stat us;
		int r = stat(q, &us) == 0 && S_ISSOCK(us.st_mode) ? 0 : 14;
		unlink(q);
		_exit(r);
	}
	waitpid(p, &w, 0);
	if (!WIFEXITED(w) || WEXITSTATUS(w)) { printf("FAIL unprivileged bind step=%d\n", WEXITSTATUS(w)); fails++; }
	rmdir("/tmp/unixperm.d");
}

/* cwd close to PATH_MAX, then a 100-byte relative name, many times */
static void longcwd(void)
{
	pid_t p = fork();
	if (p == 0) {
		char name[64];
		memset(name, 'd', 60); name[60] = 0;
		mkdir("/tmp/unixlong", 0755);
		if (chdir("/tmp/unixlong")) _exit(20);
		char cwd[PATH_MAX];
		while (getcwd(cwd, sizeof cwd) && strlen(cwd) + 62 < PATH_MAX - 4) {
			if (mkdir(name, 0755) && errno != EEXIST) break;
			if (chdir(name)) break;
		}
		if (!getcwd(cwd, sizeof cwd) || strlen(cwd) < PATH_MAX - 200) _exit(21);
		char rel[101];
		memset(rel, 's', 100); rel[100] = 0;
		for (int i = 0; i < 300; i++) {
			int s = socket(AF_UNIX, SOCK_STREAM, 0);
			struct sockaddr_un a = { .sun_family = AF_UNIX };
			memcpy(a.sun_path, rel, 101);
			int r = bind(s, (struct sockaddr *)&a, sizeof a);
			if (r == 0) { close(s); _exit(22); }   /* the name cannot be made */
			if (errno != ENAMETOOLONG) _exit(23);
			r = connect(s, (struct sockaddr *)&a, sizeof a);
			if (r == 0 || errno != ENAMETOOLONG) _exit(24);
			close(s);
		}
		_exit(0);
	}
	int w = 0;
	waitpid(p, &w, 0);
	if (!WIFEXITED(w) || WEXITSTATUS(w)) { printf("FAIL longcwd step=%d\n", WIFEXITED(w) ? WEXITSTATUS(w) : -1); fails++; }
}

int main(void)
{
	perm();
	longcwd();
	if (!fails) puts("OK");
	return fails ? 1 : 0;
}
