/* mount namespace: a child that unshares CLONE_NEWNS and mounts a private
 * tmpfs is invisible to the parent — the marker file it creates there does
 * not appear in the parent's mount namespace. */
#define _GNU_SOURCE
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <sched.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>

int main(void)
{
	mkdir("/tmp/mns", 0755);

	pid_t pid = fork();
	if (pid == 0) {
		if (unshare(CLONE_NEWNS) != 0) _exit(2);
		if (mount("tmpfs", "/tmp/mns", "tmpfs", 0, NULL) != 0) _exit(3);
		int fd = open("/tmp/mns/marker", O_CREAT | O_WRONLY, 0644);
		if (fd < 0) _exit(4);
		write(fd, "x", 1);
		close(fd);
		_exit(0);   /* the private mount dies with the namespace */
	}

	int st;
	waitpid(pid, &st, 0);
	if (!(WIFEXITED(st) && WEXITSTATUS(st) == 0)) {
		printf("mountns-fail child=%d\n", WIFEXITED(st) ? WEXITSTATUS(st) : -1);
		return 0;
	}

	/* parent is in the original namespace: the child's mount + marker must not
	 * be visible */
	int fd = open("/tmp/mns/marker", O_RDONLY);
	if (fd >= 0) { close(fd); printf("mountns-fail leaked\n"); return 0; }

	printf("mountns-ok\n");
	return 0;
}
