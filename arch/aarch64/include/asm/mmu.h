#ifndef ASM_MMU_H
#define ASM_MMU_H

/* TTBR1 (48-bit, T1SZ=16) linear map. VIRT_OFFSET sits at L0 slot 256, so the
 * kernel image + direct map span slots 256..510 (~127 TB of RAM); slot 511 is
 * left for ioremap. PHYS_TO_VIRT is a single linear offset over that range. */
#define VIRT_OFFSET     0xFFFF800000000000UL
#define PHYS_TO_VIRT(p) ((uint64_t)(p) + VIRT_OFFSET)
#define VIRT_TO_PHYS(v) ((uint64_t)(v) - VIRT_OFFSET)
#define MMIO_TO_VIRT(p) PHYS_TO_VIRT(p)

/* User space layout */
#define ARCH_USER_CODE_BASE   0x400000UL
#define ARCH_USER_STACK_TOP   0x80000000UL
#define ARCH_USER_STACK_PAGES 256
#define ARCH_USER_MMAP_BASE   0x40000000UL

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
#define PTE_NG      (1UL << 11)  /* non-global: TLB entry tagged by ASID */
#define PTE_PXN     (1UL << 53)  /* privileged execute-never */
#define PTE_UXN     (1UL << 54)  /* unprivileged execute-never */
#define PTE_NOFREE  (1UL << 55)  /* software bit: kernel-shared page, skip free on vm_destroy */

#define PTE_ATTR_DEVICE     (MT_DEVICE_nGnRnE << 2)
#define PTE_ATTR_NORMAL     (MT_NORMAL        << 2 | PTE_SHARE)
#define PTE_ATTR_NORMAL_NC  (MT_NORMAL_NC     << 2 | PTE_SHARE)

#endif
