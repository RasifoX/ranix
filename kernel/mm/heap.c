#include <kernel/kprintf.h>
#include <kernel/mm/heap.h>
#include <kernel/mm/pmm.h>

typedef struct block {
  size_t size;
  int free;
  struct block *next;
} block_t;

#define HEADER_SIZE sizeof(block_t)

static block_t *heap_head = NULL;

static block_t *heap_grow(void) {
  block_t *blk = (block_t *)pmm_alloc();
  blk->size = PAGE_SIZE - HEADER_SIZE;
  blk->free = 1;
  blk->next = NULL;
  return blk;
}

void heap_init(void) {
  heap_head = heap_grow();
}

void *kmalloc(size_t size) {
  if (size == 0)
    return NULL;
  size = (size + 7) & ~7;
  block_t *cur = heap_head;
  while (cur) {
    if (cur->free && cur->size >= size) {
      if (cur->size >= size + HEADER_SIZE + 8) {
        block_t *new = (block_t *)((uint8_t *)cur + HEADER_SIZE + size);
        new->size = cur->size - size - HEADER_SIZE;
        new->free = 1;
        new->next = cur->next;
        cur->size = size;
        cur->next = new;
      }
      cur->free = 0;
      return (void *)((uint8_t *)cur + HEADER_SIZE);
    }
    if (!cur->next) {
      cur->next = heap_grow();
    }
    cur = cur->next;
  }
  kpanic("kmalloc: heap exhausted");
  return NULL;
}

void kfree(void *ptr) {
  if (!ptr)
    return;
  block_t *blk = (block_t *)((uint8_t *)ptr - HEADER_SIZE);
  blk->free = 1;
  block_t *cur = heap_head;
  while (cur && cur->next) {
    if (cur->free && cur->next->free) {
      cur->size += HEADER_SIZE + cur->next->size;
      cur->next = cur->next->next;
    } else {
      cur = cur->next;
    }
  }
}

