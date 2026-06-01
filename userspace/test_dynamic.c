/* Dynamically linked (PT_INTERP). Reaching main and printing through libc
 * means the kernel loaded ld-musl, which relocated this binary and the shared
 * libc and jumped here. */
#include <stdio.h>

int main(void)
{
	printf("dynamic-ok\n");
	return 0;
}
