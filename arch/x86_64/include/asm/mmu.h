/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
#ifndef ASM_MMU_H
#define ASM_MMU_H

#include <uapi/types.h>

#define VIRT_OFFSET      0xFFFFFFFF80000000UL   /* kernel text link offset */
#define DIRECT_MAP_BASE  0xFFFF800000000000UL   /* physical 0..N at this VA */

/* All dynamic allocations (buddy, PMM) are accessed through the direct map. */
#define PHYS_TO_VIRT(p)  ((uint64_t)(p) + DIRECT_MAP_BASE)
/* Kernel-text statics (VA >= 0xFFFFFFFF80000000) use VIRT_OFFSET; direct-map
 * pointers (VA in [DIRECT_MAP_BASE, 0xFFFF900000000000)) use DIRECT_MAP_BASE. */
#define VIRT_TO_PHYS(v)  ((uint64_t)(v) >= 0xFFFFFFFF80000000UL \
                          ? (uint64_t)(v) - 0xFFFFFFFF80000000UL \
                          : (uint64_t)(v) - DIRECT_MAP_BASE)

/* User space layout: the lower half of 4-level paging, 128 TB, as on Linux. Runtimes
 * reserve far more address space than they use (V8's sandbox alone takes 1 TB,
 * Blink's caged heap 32 GB at a time), so the window is the whole half. User
 * page tables are PML4 slots 0..ARCH_USER_PT_SLOTS-1; the kernel's are 256..511. */
#define ARCH_USER_CODE_BASE   0x400000UL
#define ARCH_USER_STACK_TOP   0x7FFF00000000UL
#define ARCH_USER_STACK_PAGES 256
#define ARCH_USER_MMAP_BASE   0x40000000UL
#define ARCH_USER_VA_END      0x800000000000UL
#define ARCH_USER_PT_SLOTS    256
#define ARCH_HUGE1G_VA_BASE   0x7F0000000000UL   /* 1 GB-page window, below the stack */

#define PTE_VALID   (1UL << 0)   /* present */
#define PTE_WRITE   (1UL << 1)
#define PTE_USER    (1UL << 2)
#define PTE_PWT     (1UL << 3)
#define PTE_PCD     (1UL << 4)
#define PTE_AF      (1UL << 5)   /* accessed */
#define PTE_DIRTY   (1UL << 6)
#define PTE_HUGE    (1UL << 7)   /* 2 MB page at PD level */
#define PTE_GLOBAL  (1UL << 8)
#define PTE_NX      (1UL << 63)  /* no-execute (needs EFER.NXE) */

#define PTE_ATTR_NORMAL  (PTE_VALID | PTE_WRITE | PTE_AF)
#define PTE_ATTR_DEVICE  (PTE_VALID | PTE_WRITE | PTE_AF | PTE_PCD | PTE_PWT)
/* no separate NC type on x86; use PCD+PWT for uncached MMIO */
#define PTE_ATTR_NORMAL_NC  PTE_ATTR_DEVICE

#define PTE_TABLE  PTE_VALID   /* intermediate table entries just need P+W */
#define PTE_BLOCK  PTE_HUGE    /* 2 MB huge page */
#define PTE_SHARE  0UL         /* x86 has no explicit shareability bits */
#define PTE_RO     0UL         /* read-only: clear PTE_WRITE */

/* Boot page tables (boot.S); pml4/pml5 are physical addresses when cast. */
extern uint64_t pml4[], pml5[], pdpt_dm[];
extern uint8_t  la57_enabled;                    /* boot.S found 5-level paging */
void arch_vm_set_la57(int v);                    /* vm.c */

#endif
