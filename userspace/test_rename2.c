/* renameat2(2) flags: RENAME_NOREPLACE refuses to clobber an existing dest;
 * RENAME_EXCHANGE swaps the two paths' contents. */
#define _GNU_SOURCE
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/syscall.h>

#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE 1
#define RENAME_EXCHANGE  2
#endif

static int rn2(const char *a, const char *b, unsigned f)
{
	return (int)syscall(SYS_renameat2, AT_FDCWD, a, AT_FDCWD, b, f);
}

static int put(const char *p, const char *s)
{
	int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) return -1;
	write(fd, s, strlen(s));
	close(fd);
	return 0;
}

static int get(const char *p, char *buf, int n)
{
	int fd = open(p, O_RDONLY);
	if (fd < 0) return -1;
	int r = (int)read(fd, buf, n - 1);
	close(fd);
	if (r < 0) return -1;
	buf[r] = '\0';
	return 0;
}

int main(void)
{
	const char *a = "/tmp/r2_a", *b = "/tmp/r2_b";
	char buf[64];

	put(a, "AAA");
	put(b, "BBB");

	/* NOREPLACE onto an existing dest must fail EEXIST and leave both intact */
	errno = 0;
	if (rn2(a, b, RENAME_NOREPLACE) != -1 || errno != EEXIST) {
		printf("rename2-fail noreplace errno=%d\n", errno); return 0;
	}
	if (get(a, buf, sizeof buf) != 0 || strcmp(buf, "AAA")) { printf("rename2-fail nr-a\n"); return 0; }
	if (get(b, buf, sizeof buf) != 0 || strcmp(buf, "BBB")) { printf("rename2-fail nr-b\n"); return 0; }

	/* EXCHANGE swaps the contents */
	if (rn2(a, b, RENAME_EXCHANGE) != 0) { printf("rename2-fail exch errno=%d\n", errno); return 0; }
	if (get(a, buf, sizeof buf) != 0 || strcmp(buf, "BBB")) { printf("rename2-fail ex-a=%s\n", buf); return 0; }
	if (get(b, buf, sizeof buf) != 0 || strcmp(buf, "AAA")) { printf("rename2-fail ex-b=%s\n", buf); return 0; }

	/* NOREPLACE onto a free name succeeds */
	unlink("/tmp/r2_c");
	if (rn2(a, "/tmp/r2_c", RENAME_NOREPLACE) != 0) { printf("rename2-fail nr-move errno=%d\n", errno); return 0; }
	if (get("/tmp/r2_c", buf, sizeof buf) != 0 || strcmp(buf, "BBB")) { printf("rename2-fail nr-moved\n"); return 0; }

	/* bogus flag combo rejected */
	errno = 0;
	if (rn2("/tmp/r2_c", b, RENAME_NOREPLACE | RENAME_EXCHANGE) != -1 || errno != EINVAL) {
		printf("rename2-fail combo errno=%d\n", errno); return 0;
	}

	printf("OK\n");
	return 0;
}
