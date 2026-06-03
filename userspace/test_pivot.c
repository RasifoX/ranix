/* pivot_root: switch the calling process's root to a new directory. A child
 * pivots into /tmp/nr and then sees its marker as /hello, while paths outside
 * the new root (/tmp) are no longer reachable. The parent's root is untouched,
 * so this is the switch_root primitive (an init that pivots then execs the real
 * init moves the whole tree) without disturbing the rest of the system. */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>

int main(void)
{
	mkdir("/tmp/nr", 0755);
	mkdir("/tmp/nr/old", 0755);
	int fd = open("/tmp/nr/hello", O_CREAT | O_WRONLY | O_TRUNC, 0644);
	if (fd < 0) { printf("pivot-fail setup\n"); return 0; }
	write(fd, "ROOT-OK", 7);
	close(fd);

	pid_t pid = fork();
	if (pid == 0) {
		if (syscall(SYS_pivot_root, "/tmp/nr", "/tmp/nr/old") != 0) _exit(20);
		/* new root: the marker is now at /hello */
		int f = open("/hello", O_RDONLY);
		if (f < 0) _exit(21);
		char b[8] = {0};
		if (read(f, b, 7) != 7 || memcmp(b, "ROOT-OK", 7) != 0) { close(f); _exit(22); }
		close(f);
		/* /tmp is outside the new root → must not resolve */
		if (open("/tmp/nr/hello", O_RDONLY) >= 0) _exit(23);
		_exit(0);
	}

	int st;
	waitpid(pid, &st, 0);
	if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
		printf("pivot-fail child=%d\n", WIFEXITED(st) ? WEXITSTATUS(st) : -1);
		return 0;
	}

	/* parent root unaffected: the original path still works */
	int pf = open("/tmp/nr/hello", O_RDONLY);
	if (pf < 0) { printf("pivot-fail parent-root\n"); return 0; }
	close(pf);

	printf("OK\n");
	return 0;
}
