/* execveat(2): run a program both by dirfd-relative path and by an open fd
 * (AT_EMPTY_PATH). Both must exec busybox 'true' and exit 0. */
#include <stdio.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <fcntl.h>

extern char **environ;

static int run(int dirfd, const char *path, int flags)
{
	pid_t pid = fork();
	if (pid == 0) {
		char *av[] = { "busybox", "true", 0 };
		syscall(SYS_execveat, dirfd, path, av, environ, flags);
		_exit(42);   /* only reached if execveat failed */
	}
	int st = 0;
	waitpid(pid, &st, 0);
	return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

int main(void)
{
	int a = run(AT_FDCWD, "/bin/busybox", 0);

	int fd = open("/bin/busybox", O_RDONLY);
	int b = (fd >= 0) ? run(fd, "", AT_EMPTY_PATH) : -1;
	if (fd >= 0) close(fd);

	puts(a == 0 && b == 0 ? "OK" : "FAIL");
	return 0;
}
