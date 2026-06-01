#include <kernel/ksyms.h>

/* Table emitted by scripts/gen_ksyms.sh, sorted ascending by address. The weak
 * empty defaults let the stage1 link (before the table exists) resolve; the
 * generated strong definitions override them in the final link. */
__attribute__((weak)) const unsigned long ksym_count    = 0;
__attribute__((weak)) const unsigned long ksym_addrs[1] = { 0 };
__attribute__((weak)) const char *const   ksym_names[1] = { 0 };

__attribute__((weak)) const char ranix_version[]   = "0.9.0";
__attribute__((weak)) const char ranix_gitrev[]    = "unknown";
__attribute__((weak)) const char ranix_builddate[] = "?";
__attribute__((weak)) const char ranix_arch[]      = "?";

const char *ksym_lookup(uint64_t addr, uint64_t *off)
{
	if (!ksym_count || addr < ksym_addrs[0])
		return 0;

	unsigned long lo = 0, hi = ksym_count - 1, best = 0;
	while (lo <= hi) {
		unsigned long mid = (lo + hi) / 2;
		if (ksym_addrs[mid] <= addr) {
			best = mid;
			lo = mid + 1;
		} else {
			if (mid == 0) break;
			hi = mid - 1;
		}
	}
	if (off)
		*off = addr - ksym_addrs[best];
	return ksym_names[best];
}
