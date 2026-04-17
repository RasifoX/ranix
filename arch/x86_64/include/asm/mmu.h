#ifndef ASM_MMU_H
#define ASM_MMU_H

#define PTE_VALID   (1UL << 0)   /* present */
#define PTE_WRITE   (1UL << 1)
#define PTE_USER    (1UL << 2)
#define PTE_PWT     (1UL << 3)
#define PTE_PCD     (1UL << 4)
#define PTE_AF      (1UL << 5)   /* accessed */
#define PTE_DIRTY   (1UL << 6)
#define PTE_HUGE    (1UL << 7)   /* 2 MB page at PD level */
#define PTE_GLOBAL  (1UL << 8)

#define PTE_ATTR_NORMAL  (PTE_VALID | PTE_WRITE | PTE_AF)
#define PTE_ATTR_DEVICE  (PTE_VALID | PTE_WRITE | PTE_AF | PTE_PCD | PTE_PWT)
/* no separate NC type on x86; use PCD+PWT for uncached MMIO */
#define PTE_ATTR_NORMAL_NC  PTE_ATTR_DEVICE

#define PTE_TABLE  PTE_VALID   /* intermediate table entries just need P+W */
#define PTE_BLOCK  PTE_HUGE    /* 2 MB huge page */
#define PTE_SHARE  0UL         /* x86 has no explicit shareability bits */
#define PTE_RO     0UL         /* read-only: clear PTE_WRITE */

#endif
