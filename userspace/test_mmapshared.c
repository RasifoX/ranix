/* MAP_SHARED of a tmpfs file is truly shared: a store through the mapping is
 * visible via read() on a separate fd and to a second MAP_SHARED mapping, and
 * persists in the file. MAP_PRIVATE is the regression check — its stores stay
 * private (invisible to read() and to a shared mapping). */
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <sys/mman.h>

#define PG 4096

int main(void)
{
	const char *path = "/tmp/mmshared.dat";
	int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) { puts("FAIL open"); return 0; }
	if (ftruncate(fd, PG) < 0) { puts("FAIL ftruncate"); return 0; }

	char *m = mmap(NULL, PG, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (m == MAP_FAILED) { puts("FAIL mmap shared"); return 0; }

	strcpy(m, "ranix-shared");

	/* read() via a separate fd must see the store */
	int rfd = open(path, O_RDONLY);
	char buf[32] = {0};
	if (rfd < 0 || pread(rfd, buf, 12, 0) != 12 || memcmp(buf, "ranix-shared", 12) != 0) {
		puts("FAIL read sees store"); return 0;
	}

	/* a second MAP_SHARED mapping must see it too, and store back */
	char *m2 = mmap(NULL, PG, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (m2 == MAP_FAILED || memcmp(m2, "ranix-shared", 12) != 0) {
		puts("FAIL second mapping"); return 0;
	}
	m2[0] = 'R';
	if (m[0] != 'R') { puts("FAIL cross-mapping store"); return 0; }

	/* read() sees the second mapping's store */
	memset(buf, 0, sizeof buf);
	if (pread(rfd, buf, 12, 0) != 12 || memcmp(buf, "Ranix-shared", 12) != 0) {
		puts("FAIL read sees second store"); return 0;
	}

	munmap(m, PG);
	munmap(m2, PG);

	/* MAP_PRIVATE regression: stores are private, never reach the file */
	char *mp = mmap(NULL, PG, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
	if (mp == MAP_FAILED || memcmp(mp, "Ranix-shared", 12) != 0) {
		puts("FAIL private initial"); return 0;
	}
	strcpy(mp, "PRIVATE-COPY");
	memset(buf, 0, sizeof buf);
	if (pread(rfd, buf, 12, 0) != 12 || memcmp(buf, "Ranix-shared", 12) != 0) {
		puts("FAIL private leaked to file"); return 0;
	}
	char *ms = mmap(NULL, PG, PROT_READ, MAP_SHARED, fd, 0);
	if (ms == MAP_FAILED || memcmp(ms, "Ranix-shared", 12) != 0) {
		puts("FAIL private leaked to shared mapping"); return 0;
	}

	munmap(mp, PG);
	munmap(ms, PG);
	close(rfd);
	close(fd);
	unlink(path);
	puts("OK");
	return 0;
}
