/* Drive syscalls with bad user pointers: the kernel's __ex_table fixup must turn
 * a fault on the user access into -EFAULT instead of panicking. One pointer is an
 * unmapped-but-canonical user address (faults inside copy_*_user → fixup), one is
 * a kernel-range address (rejected by access_ok). Both directions are exercised:
 * getrandom() copies to user, nanosleep() copies from user. */
#include <stdio.h>
#include <stdint.h>
#include <errno.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/random.h>

#define KERNEL_PTR ((void *)0xFFFFFFFFC0000000UL)

static void *unmapped_user_page(void)
{
	void *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
	               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (p == MAP_FAILED) return NULL;
	munmap(p, 4096);
	return p;
}

static int efault(long ret) { return ret == -1 && errno == EFAULT; }

int main(void)
{
	void *bad = unmapped_user_page();
	if (!bad) { puts("FAIL mmap"); return 1; }

	if (!efault(getrandom(bad, 64, 0))) { puts("FAIL getrandom unmapped"); return 1; }
	if (!efault(getrandom(KERNEL_PTR, 64, 0))) { puts("FAIL getrandom kernel"); return 1; }

	if (!efault(nanosleep((const struct timespec *)bad, NULL))) { puts("FAIL nanosleep unmapped"); return 1; }
	if (!efault(nanosleep((const struct timespec *)KERNEL_PTR, NULL))) { puts("FAIL nanosleep kernel"); return 1; }

	char ok[64];
	if (getrandom(ok, sizeof(ok), 0) != (long)sizeof(ok)) { puts("FAIL getrandom good"); return 1; }

	puts("OK");
	return 0;
}
