/* Path resolution sweep: nested chdir + getcwd, '..' handling, relative opens,
 * and realpath canonicalization. Exercises the cwd/normalize machinery. */
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define CHECK(c) do { if (!(c)) { printf("FAIL %d %s\n", __LINE__, #c); return 0; } } while (0)

int main(void)
{
	char cwd[256];
	system("rm -rf /tmp/psw 2>/dev/null");
	CHECK(mkdir("/tmp/psw", 0755) == 0);
	CHECK(mkdir("/tmp/psw/a", 0755) == 0);
	CHECK(mkdir("/tmp/psw/a/b", 0755) == 0);

	CHECK(chdir("/tmp/psw/a/b") == 0);
	CHECK(getcwd(cwd, sizeof(cwd)) && strcmp(cwd, "/tmp/psw/a/b") == 0);

	/* ".." goes up one */
	CHECK(chdir("..") == 0);
	CHECK(getcwd(cwd, sizeof(cwd)) && strcmp(cwd, "/tmp/psw/a") == 0);

	/* "../.." goes up two */
	CHECK(chdir("../..") == 0);
	CHECK(getcwd(cwd, sizeof(cwd)) && strcmp(cwd, "/tmp") == 0);

	/* relative open against cwd (/tmp) */
	int fd = open("psw/a/b", O_RDONLY | O_DIRECTORY);
	CHECK(fd >= 0); close(fd);

	/* create + open a file relative to cwd */
	fd = open("psw/rel.txt", O_CREAT | O_WRONLY, 0644);
	CHECK(fd >= 0); write(fd, "hi", 2); close(fd);
	struct stat st;
	CHECK(stat("/tmp/psw/rel.txt", &st) == 0 && st.st_size == 2);

	/* open() itself canonicalizes '.' and '..' in a path component */
	fd = open("/tmp/psw/a/./b/../b/../b", O_RDONLY | O_DIRECTORY);
	CHECK(fd >= 0); close(fd);

	chdir("/");
	system("rm -rf /tmp/psw 2>/dev/null");
	puts("OK");
	return 0;
}
