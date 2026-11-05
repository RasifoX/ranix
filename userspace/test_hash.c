/* crc32 and sha256 are kernel code with no user interface of their own: the kernel
 * runs them against the FIPS 180-4 examples and the CRC check value at boot, and
 * reports the result here. */
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(void)
{
	static char b[4096];
	int fd = open("/dev/kmsg", O_RDONLY | O_NONBLOCK);
	if (fd < 0) { puts("FAIL kmsg"); return 1; }
	int ok = 0, bad = 0;
	ssize_t n;
	while ((n = read(fd, b, sizeof(b) - 1)) > 0) {
		b[n] = 0;
		if (strstr(b, "hash: self-test ok")) ok = 1;
		if (strstr(b, "hash: self-test FAILED")) bad = 1;
	}
	close(fd);
	if (bad || !ok) { printf("FAIL self-test ok=%d bad=%d\n", ok, bad); return 1; }
	puts("OK");
	return 0;
}
