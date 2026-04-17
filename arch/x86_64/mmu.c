#include <kernel/mm/mmu.h>

/* boot.S already sets up 1 GB identity mapping before kernel_main().
 * These functions are stubs until we set up proper per-process page tables. */

void mmu_set_ram_limit(uint64_t limit)  { (void)limit; }
void mmu_set_fb_region(uint64_t a, uint64_t b) { (void)a; (void)b; }
void mmu_init(void) {}
