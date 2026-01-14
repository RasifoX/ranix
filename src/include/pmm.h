#ifndef PMM_H
#define PMM_H

#include <stdint.h>

#define PMM_BLOCK_SIZE 4096

void pmm_init(uint32_t mem_upper_kb);

void* pmm_alloc_block();

void pmm_free_block(void* p);

#endif