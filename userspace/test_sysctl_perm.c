#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/wait.h>

/* An unprivileged process cannot change machine-wide sysctl state. */
int main(void)
{
	pid_t c = fork();
	if (c == 0) {
		if (setuid(1000) != 0) _exit(10);
		int fd = open("/proc/sys/kernel/core_pattern", O_WRONLY);
		if (fd >= 0 && write(fd, "x\n", 2) >= 0) _exit(11);
		fd = open("/proc/sys/kernel/hostname", O_WRONLY);
		if (fd >= 0 && write(fd, "x\n", 2) >= 0) _exit(12);
		_exit(0);
	}
	int st = 0; waitpid(c, &st, 0);
	if (WIFEXITED(st) && WEXITSTATUS(st) == 0) { puts("OK"); return 0; }
	printf("FAIL %d\n", WEXITSTATUS(st)); return 1;
}
