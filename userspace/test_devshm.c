/* /dev/shm must be a real tmpfs: shm_open + ftruncate + mmap + read-back. */
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>

int main(void)
{
	const char *name = "/ranix_shm_test";
	shm_unlink(name);
	int fd = shm_open(name, O_CREAT | O_RDWR, 0600);
	if (fd < 0) { puts("FAIL shm_open"); return 1; }
	if (ftruncate(fd, 4096) != 0) { puts("FAIL ftruncate"); return 1; }

	char *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (p == MAP_FAILED) { puts("FAIL mmap"); return 1; }

	strcpy(p, "ranix-shm");
	if (strcmp(p, "ranix-shm") != 0) { puts("FAIL readback"); return 1; }

	munmap(p, 4096);
	close(fd);
	shm_unlink(name);
	puts("OK");
	return 0;
}
