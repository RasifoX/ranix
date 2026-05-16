#ifndef MMU_H
#define MMU_H

#include <uapi/types.h>
#include <asm/mmu.h>

#define PAGE_SHIFT  12
#define PAGE_SIZE   (1 << PAGE_SHIFT)
#define PAGE_MASK   (~(PAGE_SIZE - 1))

void mmu_init(void);
void arch_map_high_memory(uint64_t max_phys);

#endif
