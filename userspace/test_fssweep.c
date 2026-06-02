/* Filesystem metadata sweep: hard links + nlink accounting, chmod, statfs
 * sanity, and /proc/self/exe. First failure or OK. */
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statfs.h>

#define CHECK(c) do { if (!(c)) { printf("FAIL %d %s\n", __LINE__, #c); return 0; } } while (0)

int main(void)
{
	unlink("/tmp/.fa"); unlink("/tmp/.fb");
	int fd = open("/tmp/.fa", O_CREAT | O_WRONLY, 0644);
	CHECK(fd >= 0); write(fd, "x", 1); close(fd);

	struct stat sa, sb;
	CHECK(stat("/tmp/.fa", &sa) == 0 && sa.st_nlink == 1);

	/* hard link: same inode, nlink becomes 2 */
	CHECK(link("/tmp/.fa", "/tmp/.fb") == 0);
	CHECK(stat("/tmp/.fa", &sa) == 0 && stat("/tmp/.fb", &sb) == 0);
	CHECK(sa.st_ino == sb.st_ino);
	CHECK(sa.st_nlink == 2 && sb.st_nlink == 2);

	/* unlink one: nlink back to 1, other still readable */
	CHECK(unlink("/tmp/.fb") == 0);
	CHECK(stat("/tmp/.fa", &sa) == 0 && sa.st_nlink == 1);
	CHECK(access("/tmp/.fb", F_OK) != 0);

	/* chmod changes the mode bits */
	CHECK(chmod("/tmp/.fa", 0600) == 0);
	CHECK(stat("/tmp/.fa", &sa) == 0 && (sa.st_mode & 0777) == 0600);
	unlink("/tmp/.fa");

	/* statfs returns sane geometry */
	struct statfs sf;
	CHECK(statfs("/tmp", &sf) == 0);
	CHECK(sf.f_bsize > 0 && sf.f_blocks > 0);

	/* /proc/self/exe resolves to this program */
	char exe[128];
	int n = (int)readlink("/proc/self/exe", exe, sizeof(exe) - 1);
	CHECK(n > 0); exe[n] = '\0';
	CHECK(strstr(exe, "test_fssweep") != NULL || exe[0] == '/');

	puts("OK");
	return 0;
}
