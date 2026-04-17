#include <kernel/mm/mmu.h>

static uint64_t ram_limit   = 0x3C000000UL;  /* default, overridden by DTB */
static uint64_t fb_limit    = 0x40000000UL;

void mmu_set_ram_limit(uint64_t limit) { ram_limit = limit; }
void mmu_set_fb_region(uint64_t fb_start, uint64_t fb_end)
{
    ram_limit = fb_start;
    fb_limit  = fb_end;
}

#define MMU_DEVICE_BASE_1   0x40000000UL
#define MMU_DEVICE_BASE_2   0x80000000UL
#define MMU_DEVICE_BASE_3   0xC0000000UL

__attribute__((aligned(4096))) uint64_t kernel_l1_table[512];
__attribute__((aligned(4096))) uint64_t kernel_l2_table[512];

extern void enable_mmu(uint64_t table_addr);

void mmu_init(void)
{
    for (int i = 0; i < 512; i++) {
        kernel_l1_table[i] = 0;
        kernel_l2_table[i] = 0;
    }

    kernel_l1_table[0] = (uint64_t)kernel_l2_table | PTE_TABLE | PTE_VALID;

    for (int i = 0; i < 512; i++) {
        uint64_t addr = (uint64_t)i * 0x200000;
        uint64_t attr;

        if (addr < ram_limit)
            attr = PTE_ATTR_NORMAL;
        else if (addr < fb_limit)
            attr = PTE_ATTR_NORMAL_NC;
        else
            attr = PTE_ATTR_DEVICE;

        kernel_l2_table[i] = addr | PTE_BLOCK | PTE_AF | PTE_VALID | attr;
    }

    kernel_l1_table[1] = MMU_DEVICE_BASE_1 | PTE_BLOCK | PTE_AF | PTE_VALID | PTE_ATTR_DEVICE;
    kernel_l1_table[2] = MMU_DEVICE_BASE_2 | PTE_BLOCK | PTE_AF | PTE_VALID | PTE_ATTR_DEVICE;
    kernel_l1_table[3] = MMU_DEVICE_BASE_3 | PTE_BLOCK | PTE_AF | PTE_VALID | PTE_ATTR_DEVICE;

    enable_mmu((uint64_t)kernel_l1_table);
}