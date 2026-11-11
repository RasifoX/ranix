/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* The swap areas, shared by swap.c, swapon.c and reclaim.c only. */
#ifndef KERNEL_MM_SWAP_AREA_H
#define KERNEL_MM_SWAP_AREA_H

#include <kernel/mm/swap.h>
#include <kernel/sync.h>
#include <kernel/block.h>
#include <kernel/vfs.h>

/* One swap device or file. Page 0 holds the mkswap header, so slots run
 * 1..pages-1. A slot's count is the page tables that point at it, plus one
 * while its page sits in the swap cache; 0 means free. */
typedef struct swap_area {
	int             on;          /* slots may be given out */
	int             draining;    /* swapoff is emptying it: no new slots */
	int             prio;        /* higher is used first */
	uint64_t        pages;
	uint64_t        inuse;       /* slots with a count, the header and bad pages included */
	uint64_t        bad;         /* pages the header lists as bad: never used */
	uint64_t        next;        /* allocation cursor */
	uint32_t       *count;
	uint64_t       *cache;       /* the slot's page while in RAM, else 0 */
	uint64_t       *lba;         /* a file: the sector of each slot's page; NULL: slot * 8 */
	block_device_t *bdev;
	vfs_file_t     *file;        /* held open while on */
	int             is_file;
	char            path[128];
} swap_area_t;

extern swap_area_t  swap_areas[SWAP_AREAS];
extern spinlock_t   swap_lock;      /* areas, slots, the cache; after fault_lock, before pmm_lock */
extern wait_queue_t swap_wait;      /* PG_SWAPREAD cleared */
extern uint64_t     swap_nr_in, swap_nr_out;   /* pages read and written, for /proc/vmstat */
extern uint64_t     swap_nr_cached;            /* pages in the swap cache, for SwapCached */

int  swap_io(swap_area_t *a, uint64_t slot, void *buf, int write);   /* 0 or -EIO; may sleep */
int  swap_out_pte(vm_space_t *vs, uint64_t va, uint64_t *pte, uint64_t *wb);   /* under fault_lock */
long swap_writeback(const uint64_t *pages, int n);   /* no locks held: pages freed */
long swap_writeback_pending(void);                   /* the same for pages anon_page_put left */
void swap_kswapd_start(void);

#endif
