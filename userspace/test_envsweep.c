/* Environment + exec sweep: setenv/getenv, environ has PATH, argv passed to an
 * exec'd child, and a custom envp delivered across execve. First failure or OK. */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/wait.h>

#define CHECK(c) do { if (!(c)) { printf("FAIL %d %s\n", __LINE__, #c); return 0; } } while (0)

int main(int argc, char **argv)
{
	/* internal re-exec mode: print argv + an env var, exit */
	if (argc >= 2 && !strcmp(argv[1], "--child")) {
		printf("%s|%s|%s\n", argc > 2 ? argv[2] : "", argc > 3 ? argv[3] : "",
		       getenv("RANIX_T") ? getenv("RANIX_T") : "");
		return 0;
	}

	/* setenv / getenv round-trip */
	CHECK(setenv("RANIX_T", "val42", 1) == 0);
	CHECK(getenv("RANIX_T") && !strcmp(getenv("RANIX_T"), "val42"));
	CHECK(getenv("PATH") != NULL);   /* inherited environment is populated */

	/* fork + exec self with args + a custom env; read back what the child saw */
	int fd[2];
	CHECK(pipe(fd) == 0);
	pid_t p = fork();
	if (p == 0) {
		close(fd[0]);
		dup2(fd[1], 1);
		close(fd[1]);
		char *av[] = { argv[0], "--child", "AA", "BB", NULL };
		char *ev[] = { "RANIX_T=fromenvp", "PATH=/bin", NULL };
		execve(argv[0], av, ev);
		_exit(127);
	}
	close(fd[1]);
	char buf[64];
	int n = 0, r;
	while ((r = (int)read(fd[0], buf + n, sizeof(buf) - 1 - n)) > 0) n += r;
	buf[n > 0 ? n : 0] = '\0';
	close(fd[0]);
	waitpid(p, 0, 0);

	/* child should report: argv[2]=AA, argv[3]=BB, env RANIX_T=fromenvp */
	CHECK(strstr(buf, "AA|BB|fromenvp") != NULL);

	puts("OK");
	return 0;
}
