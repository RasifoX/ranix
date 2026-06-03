/* chown/fchown actually change ownership (were no-op stubs), honour the -1
 * "don't change" sentinel, and clear the set-user/group-ID bits like Linux —
 * what tar -p, cp -a, install and package managers rely on. (Runs as root.) */
#define _GNU_SOURCE
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

int main(void)
{
	int fd = open("/tmp/chf", O_CREAT | O_RDWR | O_TRUNC, 0644);
	if (fd < 0) { printf("chown-fail create\n"); return 0; }
	chmod("/tmp/chf", 04755);   /* set the setuid bit */

	struct stat s;
	if (chown("/tmp/chf", 1000, 1000) != 0) { printf("chown-fail chown\n"); return 0; }
	if (stat("/tmp/chf", &s) != 0 || s.st_uid != 1000 || s.st_gid != 1000) {
		printf("chown-fail set u=%d g=%d\n", s.st_uid, s.st_gid); return 0;
	}
	if (s.st_mode & 04000) { printf("chown-fail setuid-not-cleared\n"); return 0; }

	/* -1 leaves a field unchanged */
	if (chown("/tmp/chf", (uid_t)-1, 0) != 0) { printf("chown-fail gidonly\n"); return 0; }
	stat("/tmp/chf", &s);
	if (s.st_uid != 1000 || s.st_gid != 0) { printf("chown-fail -1 u=%d g=%d\n", s.st_uid, s.st_gid); return 0; }

	/* fchown works via the descriptor */
	if (fchown(fd, 0, (gid_t)-1) != 0) { printf("chown-fail fchown\n"); return 0; }
	stat("/tmp/chf", &s);
	if (s.st_uid != 0 || s.st_gid != 0) { printf("chown-fail fchown-res u=%d g=%d\n", s.st_uid, s.st_gid); return 0; }
	close(fd);

	printf("OK\n");
	return 0;
}
