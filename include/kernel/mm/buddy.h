#ifndef BUDDY_H
#define BUDDY_H

#include <uapi/types.h>

#define BUDDY_MAX_ORDER  11   /* 2^10 pages = 4 MB max single alloc */

uint64_t buddy_init(uint64_t base, uint64_t size);
void     buddy_add_region(uint64_t addr, uint64_t size);
void    *buddy_alloc(int order);
void     buddy_free(uint64_t addr, int order);
uint64_t buddy_free_pages(void);

#endif
