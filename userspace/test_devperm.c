/* device-node permissions: a disk exposed at /dev/<name> is mode 0660 owned
 * by root, so an unprivileged process must be denied (EACCES) when it tries to
 * open the raw block device — only root may read/write disks directly. */
#define _GNU_SOURCE
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>

int main(void)
{
	const char *paths[] = { "/dev/sata0", "/dev/vblk0", NULL };
	const char *disk = NULL;
	for (int i = 0; paths[i]; i++) {
		struct stat st;
		if (stat(paths[i], &st) == 0 && S_ISBLK(st.st_mode)) { disk = paths[i]; break; }
	}
	if (!disk) { printf("OK\n"); return 0; } /* no disk on this target → skip */

	/* root can open the disk */
	int rfd = open(disk, O_RDONLY);
	if (rfd < 0) { printf("devperm-fail root-open errno=%d\n", errno); return 0; }
	close(rfd);

	pid_t pid = fork();
	if (pid == 0) {
		if (setgid(65534) != 0 || setuid(65534) != 0) _exit(3);
		int fd = open(disk, O_RDONLY);
		if (fd >= 0) { close(fd); _exit(1); }  /* leak: should have been denied */
		_exit(errno == EACCES ? 0 : 2);
	}
	int st;
	waitpid(pid, &st, 0);
	if (WIFEXITED(st) && WEXITSTATUS(st) == 0)
		printf("OK\n");
	else
		printf("devperm-fail code=%d\n", WIFEXITED(st) ? WEXITSTATUS(st) : -1);
	return 0;
}
