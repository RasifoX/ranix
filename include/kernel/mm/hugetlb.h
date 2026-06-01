#ifndef HUGETLB_H
#define HUGETLB_H

#include <uapi/types.h>

#define HUGE_1G_SIZE  (1UL << 30)
#define HUGE_1G_MASK  (~(HUGE_1G_SIZE - 1))

/* 1 GB hugepage pool. These blocks are 1 GB-aligned physical regions carved out
 * of RAM at boot (buddy never sees them), since the buddy allocator tops out far
 * below 1 GB. Refcounted so a block shared by fork is freed only by the last
 * referrer. */
void     hugetlb_1g_reserve(uint64_t base, int nblocks);
uint64_t huge1g_alloc(void);            /* a free block (refcount 1), or 0 */
void     huge1g_get(uint64_t pa);       /* refcount++ on the block holding pa */
void     huge1g_put(uint64_t pa);       /* refcount--; returns to pool at 0 */
int      huge1g_refcount(uint64_t pa);  /* current refcount, 0 if not pooled */
int      huge1g_owns(uint64_t pa);      /* 1 if pa lies in a pool block */

#endif
