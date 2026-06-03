/* extended attributes: set/get/list/remove on a regular file, plus the error
 * semantics tools rely on — ENODATA for missing, EEXIST under XATTR_CREATE,
 * EOPNOTSUPP for an unknown namespace, ERANGE for a too-small buffer. */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/xattr.h>

#define FILE "/tmp/xattr_test"

int main(void)
{
	int fd = open(FILE, O_CREAT | O_WRONLY | O_TRUNC, 0644);
	if (fd < 0) { printf("xattr-fail create\n"); return 0; }
	close(fd);

	char buf[64];

	/* missing attribute → ENODATA */
	errno = 0;
	if (getxattr(FILE, "user.none", buf, sizeof(buf)) != -1 || errno != ENODATA) {
		printf("xattr-fail missing errno=%d\n", errno); return 0;
	}

	/* set + get round-trip */
	if (setxattr(FILE, "user.color", "blue", 4, 0) != 0) { printf("xattr-fail set\n"); return 0; }
	long n = getxattr(FILE, "user.color", buf, sizeof(buf));
	if (n != 4 || memcmp(buf, "blue", 4) != 0) { printf("xattr-fail get n=%ld\n", n); return 0; }

	/* size query (size 0) returns the length */
	if (getxattr(FILE, "user.color", NULL, 0) != 4) { printf("xattr-fail sizeq\n"); return 0; }

	/* too-small buffer → ERANGE */
	errno = 0;
	if (getxattr(FILE, "user.color", buf, 2) != -1 || errno != ERANGE) {
		printf("xattr-fail erange errno=%d\n", errno); return 0;
	}

	/* XATTR_CREATE on an existing attr → EEXIST */
	errno = 0;
	if (setxattr(FILE, "user.color", "x", 1, XATTR_CREATE) != -1 || errno != EEXIST) {
		printf("xattr-fail create-excl errno=%d\n", errno); return 0;
	}

	/* replace value */
	if (setxattr(FILE, "user.color", "green", 5, XATTR_REPLACE) != 0) { printf("xattr-fail replace\n"); return 0; }
	n = getxattr(FILE, "user.color", buf, sizeof(buf));
	if (n != 5 || memcmp(buf, "green", 5) != 0) { printf("xattr-fail replaced\n"); return 0; }

	/* unknown namespace → EOPNOTSUPP */
	errno = 0;
	if (setxattr(FILE, "bogus.attr", "x", 1, 0) != -1 || errno != EOPNOTSUPP) {
		printf("xattr-fail ns errno=%d\n", errno); return 0;
	}

	/* a second attribute, then list must contain both names */
	if (setxattr(FILE, "user.size", "42", 2, 0) != 0) { printf("xattr-fail set2\n"); return 0; }
	char list[128];
	long ln = listxattr(FILE, list, sizeof(list));
	int saw_color = 0, saw_size = 0;
	for (long i = 0; i < ln; i += (long)strlen(list + i) + 1) {
		if (!strcmp(list + i, "user.color")) saw_color = 1;
		if (!strcmp(list + i, "user.size"))  saw_size = 1;
	}
	if (!saw_color || !saw_size) { printf("xattr-fail list ln=%ld\n", ln); return 0; }

	/* remove → then gone */
	if (removexattr(FILE, "user.color") != 0) { printf("xattr-fail rm\n"); return 0; }
	errno = 0;
	if (getxattr(FILE, "user.color", buf, sizeof(buf)) != -1 || errno != ENODATA) {
		printf("xattr-fail removed errno=%d\n", errno); return 0;
	}

	printf("OK\n");
	return 0;
}
