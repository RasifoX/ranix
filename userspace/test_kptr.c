/* kptr_restrict / dmesg_restrict sysctls: defaults are 1, root may read
 * /dev/kmsg, a non-root reader is denied while dmesg_restrict=1 and allowed once
 * it is cleared, and the sysctls round-trip through write/read. */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/wait.h>

static int read_int(const char *path)
{
	int fd = open(path, O_RDONLY);
	if (fd < 0) return -1;
	char b[16] = {0};
	int n = read(fd, b, sizeof(b) - 1);
	close(fd);
	if (n <= 0) return -1;
	return b[0] - '0';
}

static int write_str(const char *path, const char *s)
{
	int fd = open(path, O_WRONLY);
	if (fd < 0) return -1;
	int n = write(fd, s, strlen(s));
	close(fd);
	return n > 0 ? 0 : -1;
}

/* can this process read a byte from /dev/kmsg? */
static int kmsg_readable(void)
{
	int fd = open("/dev/kmsg", O_RDONLY);
	if (fd < 0) return 0;
	char c;
	int n = read(fd, &c, 1);
	close(fd);
	return n >= 0;
}

int main(void)
{
	if (read_int("/proc/sys/kernel/kptr_restrict") != 1) { printf("fail kptr-default\n"); return 0; }
	if (read_int("/proc/sys/kernel/dmesg_restrict") != 1) { printf("fail dmesg-default\n"); return 0; }

	/* root reads kmsg fine even with dmesg_restrict=1 */
	if (!kmsg_readable()) { printf("fail root-kmsg\n"); return 0; }

	/* non-root denied while restricted */
	pid_t c = fork();
	if (c == 0) {
		if (setgid(1000) != 0 || setuid(1000) != 0) _exit(2);
		_exit(kmsg_readable() ? 1 : 0);   /* 0 = correctly denied */
	}
	int st = 0; waitpid(c, &st, 0);
	if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) { printf("fail nonroot-denied (%d)\n", WEXITSTATUS(st)); return 0; }

	/* clear the restriction, non-root may now read */
	if (write_str("/proc/sys/kernel/dmesg_restrict", "0\n") != 0) { printf("fail write\n"); return 0; }
	if (read_int("/proc/sys/kernel/dmesg_restrict") != 0) { printf("fail write-roundtrip\n"); return 0; }
	c = fork();
	if (c == 0) {
		if (setgid(1000) != 0 || setuid(1000) != 0) _exit(2);
		_exit(kmsg_readable() ? 0 : 1);   /* 0 = now allowed */
	}
	waitpid(c, &st, 0);
	if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) { printf("fail nonroot-allowed (%d)\n", WEXITSTATUS(st)); return 0; }

	/* restore default */
	write_str("/proc/sys/kernel/dmesg_restrict", "1\n");

	printf("OK\n");
	return 0;
}
