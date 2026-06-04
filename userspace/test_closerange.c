/* close_range: close a contiguous fd range. Closed fds become EBADF, the fds
 * outside the range stay open; last==~0 closes everything from `first` up. */
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/syscall.h>

#ifndef SYS_close_range
#define SYS_close_range 436
#endif

static int is_open(int fd) { return fcntl(fd, F_GETFD) >= 0; }

int main(void)
{
	int a = dup(0), b = dup(0), c = dup(0), d = dup(0);
	if (a < 0 || b < 0 || c < 0 || d < 0) { puts("FAIL dup"); return 1; }

	/* close the middle pair [b,c]; a and d must survive */
	if (syscall(SYS_close_range, (unsigned)b, (unsigned)c, 0u) != 0) { puts("FAIL range"); return 1; }
	if (is_open(b) || is_open(c)) { puts("FAIL not-closed"); return 1; }
	if (!is_open(a) || !is_open(d)) { puts("FAIL over-closed"); return 1; }

	/* close everything from fd 3 up */
	if (syscall(SYS_close_range, 3u, ~0u, 0u) != 0) { puts("FAIL all"); return 1; }
	if (is_open(a) || is_open(d)) { puts("FAIL all-not-closed"); return 1; }

	puts("OK");
	return 0;
}
