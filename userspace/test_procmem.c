/* /proc/self/mem: read and write our own memory through the procfs file, using
 * the variable's address as the file offset (the gdb mechanism). */
#include <stdio.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>

int main(void)
{
	volatile uint32_t marker = 0xCAFE1234u;
	int fd = open("/proc/self/mem", O_RDWR);
	if (fd < 0) { puts("FAIL open"); return 1; }

	uint32_t v = 0;
	lseek(fd, (off_t)(uintptr_t)&marker, SEEK_SET);
	if (read(fd, &v, 4) != 4 || v != 0xCAFE1234u) { puts("FAIL read"); return 1; }

	uint32_t nv = 0xDEADBEEFu;
	lseek(fd, (off_t)(uintptr_t)&marker, SEEK_SET);
	if (write(fd, &nv, 4) != 4 || marker != 0xDEADBEEFu) { puts("FAIL write"); return 1; }

	close(fd);
	puts("OK");
	return 0;
}
