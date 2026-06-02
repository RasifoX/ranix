/* File-descriptor offset semantics: O_APPEND always writes at end, dup'd fds
 * share one offset, O_TRUNC clears on open, pwrite doesn't move the offset.
 * First failure or OK. */
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { printf("FAIL %d %s\n", __LINE__, #c); return 0; } } while (0)
#define F "/tmp/.fdsw"

int main(void)
{
	unlink(F);
	int fd = open(F, O_RDWR | O_CREAT | O_TRUNC, 0644);
	CHECK(fd >= 0);
	CHECK(write(fd, "0123456789", 10) == 10);

	/* dup shares the offset: writing via the dup advances the original's too */
	int d = dup(fd);
	CHECK(d >= 0);
	CHECK(lseek(fd, 0, SEEK_SET) == 0);
	CHECK(lseek(d, 0, SEEK_CUR) == 0);      /* dup sees the rewind */
	CHECK(write(d, "AB", 2) == 2);
	CHECK(lseek(fd, 0, SEEK_CUR) == 2);     /* original offset advanced via dup */
	close(d);
	close(fd);

	/* O_APPEND: every write lands at end regardless of lseek */
	fd = open(F, O_RDWR | O_APPEND);
	CHECK(fd >= 0);
	CHECK(lseek(fd, 0, SEEK_SET) == 0);     /* seek to start... */
	CHECK(write(fd, "XY", 2) == 2);          /* ...but append writes at end */
	char buf[32];
	int n = (int)pread(fd, buf, sizeof(buf), 0);
	CHECK(n == 12);
	CHECK(memcmp(buf, "AB23456789XY", 12) == 0);
	close(fd);

	/* O_TRUNC empties the file on open */
	fd = open(F, O_WRONLY | O_TRUNC);
	CHECK(fd >= 0);
	struct { char _[1]; } _s; (void)_s;
	off_t end = lseek(fd, 0, SEEK_END);
	CHECK(end == 0);
	close(fd);
	unlink(F);

	puts("OK");
	return 0;
}
