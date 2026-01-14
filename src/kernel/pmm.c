#include "pmm.h"
#include "string.h"
#include "stdio.h"

extern uint32_t _kernel_end;

static uint32_t *pmm_bitmap = 0;

static uint32_t used_blocks = 0;
static uint32_t max_blocks = 0;
static uint32_t bitmap_size_bytes = 0;

static void mmap_set(int bit)
{
    pmm_bitmap[bit / 32] |= (1 << (bit % 32));
}

static void mmap_unset(int bit)
{
    pmm_bitmap[bit / 32] &= ~(1 << (bit % 32));
}

static int mmap_test(int bit)
{
    return pmm_bitmap[bit / 32] & (1 << (bit % 32));
}

static int mmap_first_free()
{
    for (uint32_t i = 0; i < max_blocks / 32; i++)
    {
        if (pmm_bitmap[i] != 0xFFFFFFFF)
        {
            for (int j = 0; j < 32; j++)
            {
                int bit = 1 << j;
                if (!(pmm_bitmap[i] & bit))
                {
                    return i * 32 + j;
                }
            }
        }
    }
    return -1;
}

void pmm_init(uint32_t mem_upper_kb)
{
    uint32_t mem_total_kb = mem_upper_kb + 1024;
    max_blocks = mem_total_kb / 4;

    pmm_bitmap = (uint32_t *)&_kernel_end;

    bitmap_size_bytes = max_blocks / 8;

    memset(pmm_bitmap, 0, bitmap_size_bytes);

    for (int i = 0; i < 256; i++)
    {
        mmap_set(i);
    }

    uint32_t kernel_end_addr = (uint32_t)&_kernel_end;
    uint32_t bitmap_end_addr = kernel_end_addr + bitmap_size_bytes;

    int end_frame = (bitmap_end_addr / PMM_BLOCK_SIZE) + 1;

    for (int i = 256; i < end_frame; i++)
    {
        mmap_set(i);
    }

    used_blocks = end_frame;

    kprintf("PMM Dynamic Init:\n");
    kprintf(" - RAM: %d MB (%d Frames)\n", mem_total_kb / 1024, max_blocks);
    kprintf(" - Bitmap Address: 0x%x\n", pmm_bitmap);
    kprintf(" - Kernel+Bitmap Reserved: 0x0 -> 0x%x (%d Blocks)\n", bitmap_end_addr, end_frame);
}

void *pmm_alloc_block()
{
    if (max_blocks <= used_blocks)
        return 0;

    int frame = mmap_first_free();
    if (frame == -1)
        return 0;

    mmap_set(frame);
    used_blocks++;

    uint32_t addr = frame * PMM_BLOCK_SIZE;
    return (void *)((uintptr_t)addr);
}

void pmm_free_block(void *p)
{
    uintptr_t addr = (uintptr_t)p;
    int frame = addr / PMM_BLOCK_SIZE;

    uint32_t kernel_limit = ((uint32_t)&_kernel_end) + bitmap_size_bytes;

    if (addr < kernel_limit)
    {
        kprintf("PMM Warn: Refusing to free kernel memory (0x%x)\n", addr);
        return;
    }

    mmap_unset(frame);
    used_blocks--;
}