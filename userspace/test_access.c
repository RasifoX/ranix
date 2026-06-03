/* access(2) honours R_OK/W_OK/X_OK (was existence-only). Root is denied X_OK on
 * a file with no execute bit; an unprivileged process is denied R/W on a
 * root-owned 0600 file. configure scripts and shell test -r/-w/-x rely on this. */
#define _GNU_SOURCE
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>

int main(void)
{
	int fd = open("/tmp/af", O_CREAT | O_RDWR | O_TRUNC, 0644);
	if (fd < 0) { printf("access-fail create\n"); close(fd); return 0; }
	close(fd);
	chmod("/tmp/af", 0644);   /* no execute bit */

	if (access("/tmp/af", F_OK) != 0) { printf("access-fail fok\n"); return 0; }
	errno = 0;
	if (access("/tmp/af", X_OK) != -1 || errno != EACCES) { printf("access-fail xok errno=%d\n", errno); return 0; }
	chmod("/tmp/af", 0755);
	if (access("/tmp/af", X_OK) != 0) { printf("access-fail xok2\n"); return 0; }
	errno = 0;
	if (access("/tmp/no_such_xyz", F_OK) != -1 || errno != ENOENT) { printf("access-fail enoent\n"); return 0; }

	/* root-owned 0600 file; an unprivileged child must be denied R/W */
	int f2 = open("/tmp/af2", O_CREAT | O_RDWR | O_TRUNC, 0600);
	if (f2 >= 0) close(f2);
	chmod("/tmp/af2", 0600);

	pid_t pid = fork();
	if (pid == 0) {
		if (setgid(65534) != 0 || setuid(65534) != 0) _exit(20);
		errno = 0;
		if (access("/tmp/af2", W_OK) != -1 || errno != EACCES) _exit(21);
		errno = 0;
		if (access("/tmp/af2", R_OK) != -1 || errno != EACCES) _exit(22);
		_exit(0);
	}
	int st; waitpid(pid, &st, 0);
	if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) { printf("access-fail nonroot code=%d\n", WIFEXITED(st) ? WEXITSTATUS(st) : -1); return 0; }

	printf("OK\n");
	return 0;
}
