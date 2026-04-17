#ifndef HEAP_H
#define HEAP_H

#include <uapi/types.h>

void heap_init(void);
void *kmalloc(size_t size);
void kfree(void *ptr);

#endif
