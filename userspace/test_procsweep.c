/* Process/wait/exec/pipe edge sweep: exit-status encoding, signal-death status,
 * execve exit codes, WNOHANG, parent/child pid relations, nonblocking pipe
 * EAGAIN, and pipe EOF. First failure or OK. */
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <string.h>
#include <sys/wait.h>

#define CHECK(c) do { if (!(c)) { printf("FAIL %d %s\n", __LINE__, #c); return 0; } } while (0)

int main(void)
{
	int st;
	pid_t me = getpid();

	/* exit code round-trips through wait status */
	pid_t p = fork();
	if (p == 0) _exit(42);
	CHECK(waitpid(p, &st, 0) == p);
	CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 42);

	/* child's getppid is us */
	int pfd[2]; CHECK(pipe(pfd) == 0);
	p = fork();
	if (p == 0) { close(pfd[0]); pid_t pp = getppid(); write(pfd[1], &pp, sizeof(pp)); _exit(0); }
	close(pfd[1]);
	pid_t seen; CHECK(read(pfd[0], &seen, sizeof(seen)) == sizeof(seen));
	close(pfd[0]); waitpid(p, 0, 0);
	CHECK(seen == me);

	/* death by signal is reported as WIFSIGNALED */
	p = fork();
	if (p == 0) { raise(SIGTERM); _exit(0); }
	CHECK(waitpid(p, &st, 0) == p);
	CHECK(WIFSIGNALED(st) && WTERMSIG(st) == SIGTERM);

	/* execve exit codes: /bin/true -> 0, /bin/false -> 1 */
	p = fork();
	if (p == 0) { execl("/bin/true", "true", (char*)0); _exit(127); }
	CHECK(waitpid(p, &st, 0) == p && WIFEXITED(st) && WEXITSTATUS(st) == 0);
	p = fork();
	if (p == 0) { execl("/bin/false", "false", (char*)0); _exit(127); }
	CHECK(waitpid(p, &st, 0) == p && WIFEXITED(st) && WEXITSTATUS(st) == 1);

	/* WNOHANG returns 0 while the child is still alive */
	int gate[2]; CHECK(pipe(gate) == 0);
	p = fork();
	if (p == 0) { close(gate[1]); char c; read(gate[0], &c, 1); _exit(0); }
	CHECK(waitpid(p, &st, WNOHANG) == 0);   /* not exited yet */
	close(gate[1]);                          /* release the child */
	CHECK(waitpid(p, &st, 0) == p && WIFEXITED(st));
	close(gate[0]);

	/* nonblocking pipe: read on empty returns EAGAIN */
	int nb[2]; CHECK(pipe(nb) == 0);
	fcntl(nb[0], F_SETFL, fcntl(nb[0], F_GETFL) | O_NONBLOCK);
	char b;
	CHECK(read(nb[0], &b, 1) == -1 && errno == EAGAIN);
	/* after writing, the byte is readable */
	CHECK(write(nb[1], "x", 1) == 1);
	CHECK(read(nb[0], &b, 1) == 1 && b == 'x');
	/* closing the write end yields EOF (0) */
	close(nb[1]);
	CHECK(read(nb[0], &b, 1) == 0);
	close(nb[0]);

	puts("OK");
	return 0;
}
