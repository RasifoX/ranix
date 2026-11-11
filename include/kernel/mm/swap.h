/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* Swap: private anonymous pages go out to a swap area when memory runs short
 * and come back on the next touch. Design: arastirma/swap.md.
 *   kernel/mm/swap.c     slots, the swap cache, swap-in, page-table entries
 *   kernel/mm/swapon.c   swapon/swapoff, the mkswap header, /proc/swaps
 *   kernel/mm/reclaim.c  which pages go: a second-chance clock, kswapd */
#ifndef KERNEL_MM_SWAP_H
#define KERNEL_MM_SWAP_H

#include <uapi/types.h>
#include <kernel/mm/vm.h>

/* A swapped-out page in a page table: not present (bit 0 clear, so the MMU
 * ignores the rest on x86 and aarch64 alike), bit 1 set, the area in bits 2-6,
 * the slot inverted in bits 12-51. Inverted, the address field of the entry
 * points far above any RAM: a CPU that reads through a not-present entry
 * speculatively (Intel L1TF) finds nothing there, as Linux arranges too. */
#define SWP_PTE_MARK  (1UL << 1)
#define SWP_SLOT_MASK 0xFFFFFFFFFFUL
#define SWAP_AREAS    8

static inline int      swp_pte_is(uint64_t e)   { return (e & 3) == SWP_PTE_MARK; }
static inline unsigned swp_area(uint64_t e)     { return (unsigned)(e >> 2) & 0x1F; }
static inline uint64_t swp_slot(uint64_t e)     { return ~(e >> 12) & SWP_SLOT_MASK; }
static inline uint64_t swp_make(unsigned area, uint64_t slot)
{
	return ((~slot & SWP_SLOT_MASK) << 12) | ((uint64_t)area << 2) | SWP_PTE_MARK;
}

/* Page tables (the arch vm.c files and the fault paths call these) */
void swap_pte_dup(uint64_t e);        /* fork copied a swap entry */
int  swap_pte_free(uint64_t e);       /* a swap entry left a page table; 1: its page is gone */
int  anon_page_put(uint64_t pa);      /* pmm_free for a user page; 1: gone, swap cache too */
void anon_page_mark(uint64_t pa);     /* a private anonymous page: it may be swapped */
int  swap_fault(vm_space_t *vs, uint64_t va, uint64_t e, int is_write);   /* 0, or -1 */
int  swap_fault_at(vm_space_t *vs, uint64_t va);   /* swap the page at va in if it is out: 1 in, 0 not out, -1 */
int  swap_pte_at(vm_space_t *vs, uint64_t va);     /* 1: the page at va is out on swap */
void swap_in_all(vm_space_t *vs);                  /* read every page vs has out back in (mlockall) */
int  swap_user_retry(uint64_t uaddr, size_t len);  /* a user access with IRQs off failed: read
                                                    * the range in with IRQs on; 1: retry it */

/* Reclaim (kernel/mm/reclaim.c) */
#define RECLAIM_ANON  1   /* may swap anonymous pages (else only caches) */
int  swap_active(void);                      /* some swap area is on */
long swap_reclaim(long want, int flags);     /* pages freed */
void swap_memory_check(void);                /* fault entry: wake kswapd, reclaim directly when low */
long swap_pageout(vm_space_t *vs, uint64_t start, uint64_t end);   /* MADV_PAGEOUT; pages swapped */
extern int vm_swappiness;                    /* /proc/sys/vm/swappiness, 0..200 */
long swap_test_hook_set(uint64_t on);        /* PR_RANIX_TEST_HOOK: test kernels only */
void swap_test_race(vm_space_t *vs, uint64_t va);   /* the hook point: no lock held */

/* Swapon (kernel/mm/swapon.c) */
long swap_on(const char *kpath, int flags);
long swap_off(const char *kpath);
int  swap_proc_swaps(char *buf, size_t sz);
void swap_totals(uint64_t *total, uint64_t *free, uint64_t *cached);   /* bytes */
void swap_counts(uint64_t *pswpin, uint64_t *pswpout);
uint64_t swap_vm_bytes(vm_space_t *vs);      /* VmSwap: bytes of vs out on swap */

/* zram (drivers/block/zram.c) */
struct block_device;
void     zram_init(void);
int      zram_owns(struct block_device *dev);
void     zram_discard(struct block_device *dev, uint64_t lba);   /* a page's storage is not needed */
int      zram_set_disksize(uint64_t bytes);                      /* 0 or -EBUSY/-ENOMEM */
uint64_t zram_disksize(void);
void     zram_mm_stat(uint64_t v[8]);                            /* /sys/block/zram0/mm_stat */

#endif
