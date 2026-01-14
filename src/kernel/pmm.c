#include "pmm.h"
#include "string.h"
#include "stdio.h"

#define MAX_RAM_FRAMES 65536
static uint32_t pmm_bitmap[MAX_RAM_FRAMES / 32];

static uint32_t used_blocks = 0;
static uint32_t max_blocks = 0;

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

    if (max_blocks > MAX_RAM_FRAMES)
    {
        max_blocks = MAX_RAM_FRAMES;
        kprintf("WARNING: Detected RAM > Supported RAM. Clamping to 256MB.\n");
    }

    used_blocks = max_blocks;

    memset(pmm_bitmap, 0, sizeof(pmm_bitmap));

    for (int i = 0; i < 256; i++)
    {
        mmap_set(i);
    }

    used_blocks = 256;

    kprintf("PMM Initialized:\n");
    kprintf(" - Total RAM: %d MB\n", mem_total_kb / 1024);
    kprintf(" - Managed Blocks: %d\n", max_blocks);
    kprintf(" - Reserved Blocks: %d (First 1MB)\n", used_blocks);
}

void *pmm_alloc_block()
{
    if (max_blocks <= used_blocks)
    {
        kprintf("PMM: Out of Memory!\n");
        return 0;
    }

    int frame = mmap_first_free();
    if (frame == -1)
    {
        kprintf("PMM: No free frames found!\n");
        return 0;
    }

    mmap_set(frame);
    used_blocks++;

    uint32_t addr = frame * PMM_BLOCK_SIZE;
    return (void *)((uintptr_t)addr);
}

void pmm_free_block(void *p)
{
    uintptr_t addr = (uintptr_t)p;
    int frame = addr / PMM_BLOCK_SIZE;

    if (frame >= (int)max_blocks)
    {
        kprintf("PMM Warning: Attempted to free out-of-bounds address: 0x%x\n", addr);
        return;
    }

    if (frame < 256)
    {
        kprintf("PMM Warning: Attempted to free reserved kernel memory: 0x%x\n", addr);
        return;
    }

    mmap_unset(frame);
    used_blocks--;
}