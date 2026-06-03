/* POSIX fcntl record locks (F_SETLK/F_GETLK): byte-range advisory locks across
 * processes — overlapping write locks conflict (EAGAIN), non-overlapping ones
 * coexist, F_GETLK reports the holder, and locks release on close. (sqlite,
 * dpkg, daemons.) */
#define _GNU_SOURCE
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/wait.h>

static struct flock mk(short type, off_t start, off_t len)
{
	struct flock f; f.l_type = type; f.l_whence = SEEK_SET;
	f.l_start = start; f.l_len = len; f.l_pid = 0; return f;
}

int main(void)
{
	int fd = open("/tmp/lkf", O_CREAT | O_RDWR | O_TRUNC, 0644);
	if (fd < 0) { printf("fcntllock-fail open\n"); return 0; }

	struct flock fl = mk(F_WRLCK, 0, 10);
	if (fcntl(fd, F_SETLK, &fl) != 0) { printf("fcntllock-fail lock\n"); return 0; }

	pid_t pid = fork();
	if (pid == 0) {
		int c = open("/tmp/lkf", O_RDWR);
		if (c < 0) _exit(2);
		struct flock cf = mk(F_WRLCK, 0, 10);          /* overlaps parent → conflict */
		errno = 0;
		if (fcntl(c, F_SETLK, &cf) != -1 || errno != EAGAIN) _exit(3);
		struct flock nf = mk(F_WRLCK, 20, 10);         /* disjoint → granted */
		if (fcntl(c, F_SETLK, &nf) != 0) _exit(4);
		struct flock gf = mk(F_WRLCK, 0, 10);          /* who holds 0..9? */
		if (fcntl(c, F_GETLK, &gf) != 0) _exit(5);
		if (gf.l_type != F_WRLCK || gf.l_pid != getppid()) _exit(6);
		_exit(0);
	}
	int st; waitpid(pid, &st, 0);
	if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) { printf("fcntllock-fail child=%d\n", WIFEXITED(st) ? WEXITSTATUS(st) : -1); return 0; }

	/* closing the fd releases our lock; a fresh fd can then re-lock 0..9 */
	close(fd);
	int fd2 = open("/tmp/lkf", O_RDWR);
	struct flock rl = mk(F_WRLCK, 0, 10);
	if (fcntl(fd2, F_SETLK, &rl) != 0) { printf("fcntllock-fail release-on-close\n"); return 0; }
	close(fd2);

	printf("OK\n");
	return 0;
}
