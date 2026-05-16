#ifndef ASM_MMU_H
#define ASM_MMU_H

#define VIRT_OFFSET     0xFFFFFF8000000000UL
#define PHYS_TO_VIRT(p) ((uint64_t)(p) + VIRT_OFFSET)
#define VIRT_TO_PHYS(v) ((uint64_t)(v) - VIRT_OFFSET)
/* aarch64 VIRT_OFFSET covers all 512GB — PHYS_TO_VIRT always safe */
#define MMIO_TO_VIRT(p) PHYS_TO_VIRT(p)

/* User space layout */
#define ARCH_USER_CODE_BASE   0x400000UL
#define ARCH_USER_STACK_TOP   0x800000UL
#define ARCH_USER_STACK_PAGES 16
#define ARCH_USER_MMAP_BASE   0x10000000UL

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
#define PTE_ATTR_NORMAL_NC  (MT_NORMAL_NC     << 2 | PTE_SHARE)

#endif
