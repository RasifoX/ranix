#ifndef BUDDY_H
#define BUDDY_H

#include <uapi/types.h>

#define BUDDY_MAX_ORDER  14   /* 2^13 pages = 32 MB max single alloc */

uint64_t buddy_init(uint64_t base, uint64_t size);
/* Allocate the per-CPU order-0 magazine caches (sized to ncpus) and enable the
 * lock-free fast path. Call once after the heap and CPU count are known. */
void     buddy_init_percpu(int ncpus);
void     buddy_add_region(uint64_t addr, uint64_t size);
void    *buddy_alloc(int order);
void     buddy_free(uint64_t addr, int order);
uint64_t buddy_free_pages(void);
uint64_t buddy_total_pages(void);

#endif
