/* execve(2) #! interpreter handling + execute-permission enforcement:
 *  - a #!/bin/sh script run via execve is handed to the interpreter;
 *  - argv[1] passed by the shebang line reaches the interpreter as the script;
 *  - a script without the execute bit fails EACCES. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <sys/wait.h>
#include <sys/stat.h>

static int write_file(const char *path, const char *data, mode_t mode)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
	if (fd < 0) return -1;
	size_t n = strlen(data);
	if (write(fd, data, n) != (ssize_t)n) { close(fd); return -1; }
	close(fd);
	chmod(path, mode);
	return 0;
}

int main(void)
{
	const char *script = "/tmp/sb_script.sh";
	if (write_file(script, "#!/bin/sh\necho shebang-ran\n", 0755) != 0) {
		printf("shebang-fail write\n"); return 0;
	}

	pid_t pid = fork();
	if (pid == 0) {
		int dn = open("/dev/null", O_WRONLY);
		if (dn >= 0) { dup2(dn, 1); dup2(dn, 2); }
		char *av[] = { (char *)script, NULL };
		char *ev[] = { NULL };
		execve(script, av, ev);
		_exit(127);              /* execve failed */
	}
	int st;
	waitpid(pid, &st, 0);
	if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
		printf("shebang-fail exec exit=%d\n", WIFEXITED(st) ? WEXITSTATUS(st) : -1);
		return 0;
	}

	/* non-executable script must be rejected with EACCES */
	const char *nox = "/tmp/sb_nox.sh";
	if (write_file(nox, "#!/bin/sh\necho nope\n", 0644) != 0) {
		printf("shebang-fail write2\n"); return 0;
	}
	chmod(nox, 0644);
	char *av2[] = { (char *)nox, NULL };
	char *ev2[] = { NULL };
	errno = 0;
	execve(nox, av2, ev2);
	if (errno != EACCES) { printf("shebang-fail noexec errno=%d\n", errno); return 0; }

	printf("OK\n");
	return 0;
}
