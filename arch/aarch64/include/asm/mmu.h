#ifndef ASM_MMU_H
#define ASM_MMU_H

/* MAIR indices — must match mmu_asm.S enable_mmu() setup */
#define MT_DEVICE_nGnRnE  0x0
#define MT_NORMAL         0x1
#define MT_NORMAL_NC      0x2

#define PTE_VALID   (1UL << 0)
#define PTE_TABLE   (1UL << 1)
#define PTE_BLOCK   (0UL << 1)
#define PTE_USER    (1UL << 6)
#define PTE_RO      (1UL << 7)
#define PTE_SHARE   (3UL << 8)
#define PTE_AF      (1UL << 10)

#define PTE_ATTR_DEVICE     (MT_DEVICE_nGnRnE << 2)
#define PTE_ATTR_NORMAL     (MT_NORMAL        << 2 | PTE_SHARE)
#define PTE_ATTR_NORMAL_NC  (MT_NORMAL_NC     << 2)

#endif
