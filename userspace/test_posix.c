/* Broad POSIX behaviour sweep: exercises a pile of small syscall semantics that
 * tools quietly depend on. Prints the first failure (so a regression is
 * pinpointed) or OK if every check holds. */
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/uio.h>

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d %s\n", __func__, __LINE__, #cond); return 0; } } while (0)

int main(void)
{
	/* getcwd / chdir round-trip */
	char cwd[256];
	CHECK(getcwd(cwd, sizeof(cwd)) != NULL);
	CHECK(chdir("/tmp") == 0);
	char cwd2[256];
	CHECK(getcwd(cwd2, sizeof(cwd2)) && strcmp(cwd2, "/tmp") == 0);
	chdir(cwd);

	/* open / write / lseek / pread / fstat / ftruncate */
	int fd = open("/tmp/.posix_t", O_RDWR | O_CREAT | O_TRUNC, 0644);
	CHECK(fd >= 0);
	CHECK(write(fd, "hello world", 11) == 11);
	CHECK(lseek(fd, 0, SEEK_SET) == 0);
	CHECK(lseek(fd, 0, SEEK_END) == 11);
	char rb[16];
	CHECK(pread(fd, rb, 5, 6) == 5 && memcmp(rb, "world", 5) == 0);
	struct stat st;
	CHECK(fstat(fd, &st) == 0 && st.st_size == 11 && S_ISREG(st.st_mode));
	CHECK(ftruncate(fd, 4) == 0);
	CHECK(fstat(fd, &st) == 0 && st.st_size == 4);

	/* fcntl: FD_CLOEXEC, O_NONBLOCK, F_DUPFD */
	CHECK(fcntl(fd, F_SETFD, FD_CLOEXEC) == 0);
	CHECK((fcntl(fd, F_GETFD) & FD_CLOEXEC) != 0);
	int fl = fcntl(fd, F_GETFL);
	CHECK(fcntl(fd, F_SETFL, fl | O_NONBLOCK) == 0);
	CHECK((fcntl(fd, F_GETFL) & O_NONBLOCK) != 0);
	int fd2 = fcntl(fd, F_DUPFD, 10);
	CHECK(fd2 >= 10);
	close(fd2);

	/* dup2 */
	int fd3 = dup2(fd, 20);
	CHECK(fd3 == 20);
	close(fd3);
	close(fd);

	/* readv / writev */
	int fv = open("/tmp/.posix_v", O_RDWR | O_CREAT | O_TRUNC, 0644);
	CHECK(fv >= 0);
	struct iovec wv[2] = { {"ab", 2}, {"cde", 3} };
	CHECK(writev(fv, wv, 2) == 5);
	lseek(fv, 0, SEEK_SET);
	char b1[2], b2[3];
	struct iovec rv[2] = { {b1, 2}, {b2, 3} };
	CHECK(readv(fv, rv, 2) == 5 && b1[0] == 'a' && b2[2] == 'e');
	close(fv);
	unlink("/tmp/.posix_v");

	/* symlink / readlink / lstat */
	unlink("/tmp/.posix_l");
	CHECK(symlink("/tmp/.posix_t", "/tmp/.posix_l") == 0);
	char lb[64];
	int ln = (int)readlink("/tmp/.posix_l", lb, sizeof(lb) - 1);
	CHECK(ln > 0); lb[ln] = '\0';
	CHECK(strcmp(lb, "/tmp/.posix_t") == 0);
	CHECK(lstat("/tmp/.posix_l", &st) == 0 && S_ISLNK(st.st_mode));
	unlink("/tmp/.posix_l");

	/* mkdir / rename / rmdir */
	rmdir("/tmp/.posix_d2"); rmdir("/tmp/.posix_d1");
	CHECK(mkdir("/tmp/.posix_d1", 0755) == 0);
	CHECK(rename("/tmp/.posix_d1", "/tmp/.posix_d2") == 0);
	CHECK(rmdir("/tmp/.posix_d2") == 0);

	/* access */
	CHECK(access("/tmp/.posix_t", F_OK) == 0);
	CHECK(access("/tmp/.nope_xyz", F_OK) != 0);
	unlink("/tmp/.posix_t");
	CHECK(access("/tmp/.posix_t", F_OK) != 0);

	/* umask round-trip */
	mode_t old = umask(022);
	CHECK(umask(old) == 022);

	puts("OK");
	return 0;
}
