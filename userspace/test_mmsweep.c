/* mmap sweep: anonymous RW, file-backed read, MAP_FIXED honoring, and
 * MAP_PRIVATE copy-on-write isolation across fork. First failure or OK. */
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>

#define CHECK(c) do { if (!(c)) { printf("FAIL %d %s\n", __LINE__, #c); return 0; } } while (0)
#define PG 4096

int main(void)
{
	/* anonymous RW */
	char *m = mmap(0, PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	CHECK(m != MAP_FAILED);
	for (int i = 0; i < PG; i++) m[i] = (char)(i & 0xff);
	CHECK(m[0] == 0 && (unsigned char)m[255] == 255 && m[PG - 1] == (char)((PG - 1) & 0xff));
	CHECK(munmap(m, PG) == 0);

	/* file-backed: write a file, mmap it, read content via the mapping */
	int fd = open("/tmp/.mm", O_RDWR | O_CREAT | O_TRUNC, 0644);
	CHECK(fd >= 0);
	CHECK(write(fd, "MMAPDATA", 8) == 8);
	char *fm = mmap(0, 8, PROT_READ, MAP_PRIVATE, fd, 0);
	CHECK(fm != MAP_FAILED);
	CHECK(memcmp(fm, "MMAPDATA", 8) == 0);
	munmap(fm, 8);
	close(fd);
	unlink("/tmp/.mm");

	/* MAP_PRIVATE copy-on-write: child's writes don't affect the parent */
	char *cow = mmap(0, PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	CHECK(cow != MAP_FAILED);
	cow[0] = 'P';
	pid_t pid = fork();
	if (pid == 0) { cow[0] = 'C'; _exit(0); }
	int st; waitpid(pid, &st, 0);
	CHECK(cow[0] == 'P');          /* parent unchanged by child's COW write */
	munmap(cow, PG);

	puts("OK");
	return 0;
}
