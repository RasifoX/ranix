/* membarrier(2): QUERY returns a sane non-zero mask and an expedited command
 * completes (0, or a consistent ENOSYS) without corrupting or hanging. */
#define _GNU_SOURCE
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <linux/membarrier.h>

static long membarrier(int cmd, unsigned int flags)
{
	return syscall(SYS_membarrier, cmd, flags, 0);
}

int main(void)
{
	long mask = membarrier(MEMBARRIER_CMD_QUERY, 0);
	if (mask <= 0) { printf("membarrier-fail query=%ld\n", mask); return 0; }

	long r = membarrier(MEMBARRIER_CMD_PRIVATE_EXPEDITED, 0);
	if (r != 0 && !(r == -1 && errno == ENOSYS)) {
		printf("membarrier-fail private=%ld errno=%d\n", r, errno);
		return 0;
	}

	long g = membarrier(MEMBARRIER_CMD_GLOBAL, 0);
	if (g != 0 && !(g == -1 && errno == ENOSYS)) {
		printf("membarrier-fail global=%ld errno=%d\n", g, errno);
		return 0;
	}

	printf("OK\n");
	return 0;
}
