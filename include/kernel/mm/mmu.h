#ifndef MMU_H
#define MMU_H

#include <uapi/types.h>
#include <asm/mmu.h>

#define PAGE_SHIFT  12
#define PAGE_SIZE   (1 << PAGE_SHIFT)
#define PAGE_MASK   (~(PAGE_SIZE - 1))

void mmu_init(void);
void mmu_set_ram_limit(uint64_t limit);
void mmu_set_fb_region(uint64_t fb_start, uint64_t fb_end);

#endif
