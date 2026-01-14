#include "heap.h"
#include "stdio.h" 

static heap_block_t *heap_head = 0;

void kheap_init(void* start_addr, size_t size)
{
    heap_head = (heap_block_t*)start_addr;

    heap_head->size = size - sizeof(heap_block_t);
    heap_head->is_free = 1;
    heap_head->next = 0;

    kprintf("Heap Initialized at 0x%x (Size: %d bytes)\n", start_addr, size);
}

void* kmalloc(size_t size)
{
    if (size == 0) return 0;

    heap_block_t *current = heap_head;
    while (current) {
        if (current->is_free && current->size >= size) {
            if (current->size > size + sizeof(heap_block_t)) {

                heap_block_t *new_block = (heap_block_t*)((uintptr_t)current + sizeof(heap_block_t) + size);
                
                new_block->size = current->size - size - sizeof(heap_block_t);
                new_block->is_free = 1;
                new_block->next = current->next;

                current->size = size;
                current->next = new_block;
            }

            current->is_free = 0;

            return (void*)((uintptr_t)current + sizeof(heap_block_t));
        }
        current = current->next;
    }

    kprintf("Heap: Out of memory! Requested: %d bytes\n", size);
    return 0;
}

void kfree(void* ptr)
{
    if (!ptr) return;

    heap_block_t *block = (heap_block_t*)((uintptr_t)ptr - sizeof(heap_block_t));

    block->is_free = 1;

    if (block->next && block->next->is_free) {
        block->size += block->next->size + sizeof(heap_block_t);
        
        block->next = block->next->next;
    }
}