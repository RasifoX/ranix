/* V8-cage acceptance check: a single 4 GB PROT_NONE reservation must succeed
 * (it returned ENOMEM under the old 2 GB ceiling), then sub-regions mprotect to
 * RW and are touchable without SIGSEGV, then munmap cleanly. Mirrors how V8's
 * pointer-compression heap cage reserves its address range up front. */
#include <stdio.h>
#include <sys/mman.h>
#include <stdint.h>

int main(void)
{
	size_t cage = 4UL * 1024 * 1024 * 1024;   /* 4 GB */

	char *base = mmap(NULL, cage, PROT_NONE,
	                  MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
	if (base == MAP_FAILED) { puts("FAIL reserve"); return 0; }

	/* commit a window near the start and one near the 3 GB mark */
	size_t win = 2UL * 1024 * 1024;
	char *lo = base;
	char *hi = base + 3UL * 1024 * 1024 * 1024;
	if (mprotect(lo, win, PROT_READ | PROT_WRITE) < 0) { puts("FAIL prot-lo"); return 0; }
	if (mprotect(hi, win, PROT_READ | PROT_WRITE) < 0) { puts("FAIL prot-hi"); return 0; }

	for (size_t i = 0; i < win; i += 4096) { lo[i] = (char)i; hi[i] = (char)~i; }
	for (size_t i = 0; i < win; i += 4096)
		if (lo[i] != (char)i || hi[i] != (char)~i) { puts("FAIL readback"); return 0; }

	if (munmap(base, cage) < 0) { puts("FAIL unmap"); return 0; }

	/* a plain large anonymous mapping must also work */
	char *big = mmap(NULL, 256UL * 1024 * 1024, PROT_READ | PROT_WRITE,
	                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (big == MAP_FAILED) { puts("FAIL big"); return 0; }
	big[0] = 1; big[256UL * 1024 * 1024 - 1] = 2;
	if (munmap(big, 256UL * 1024 * 1024) < 0) { puts("FAIL big-unmap"); return 0; }

	puts("OK");
	return 0;
}
