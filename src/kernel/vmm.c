#include <stdint.h>
#include "vmm.h"
#include "pmm.h"
#include "string.h"
#include "stdio.h"

extern uint32_t _kernel_start;
extern uint32_t _kernel_end;

uint32_t page_directory[1024] __attribute__((aligned(4096)));
uint32_t first_page_table[1024] __attribute__((aligned(4096)));

#define PD_INDEX(vaddr) ((uint32_t)vaddr >> 22)
#define PT_INDEX(vaddr) (((uint32_t)vaddr >> 12) & 0x03FF)

void vmm_enable_paging()
{
    asm volatile("mov %0, %%cr3" : : "r"(&page_directory));
    uint32_t cr0;
    asm volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= 0x80000000;
    asm volatile("mov %0, %%cr0" : : "r"(cr0));
}

int vmm_map_page(void *phys, void *virt)
{
    uint32_t pd_index = PD_INDEX(virt);
    uint32_t pt_index = PT_INDEX(virt);

    uint32_t *pde = &page_directory[pd_index];

    if (!(*pde & I86_PDE_PRESENT))
    {
        uint32_t new_pt_phys = (uint32_t)pmm_alloc_block();
        if (!new_pt_phys)
            return 0;

        memset((void *)new_pt_phys, 0, 4096);
        *pde = new_pt_phys | I86_PDE_PRESENT | I86_PDE_WRITABLE;
    }

    uint32_t *pt = (uint32_t *)(*pde & I86_PDE_FRAME);
    pt[pt_index] = ((uint32_t)phys) | I86_PTE_PRESENT | I86_PTE_WRITABLE;

    return 1;
}

void vmm_init()
{
    for (int i = 0; i < 1024; i++)
    {
        page_directory[i] = I86_PDE_WRITABLE;
    }

    uint32_t kernel_limit = (uint32_t)&_kernel_end;
    kernel_limit += 1024 * 1024;

    uint32_t total_pages = (kernel_limit + 4095) / 4096;

    if (total_pages > 1024)
    {
        kprintf("VMM Error: Kernel too big for initial page table!\n");

        total_pages = 1024;
    }

    for (uint32_t i = 0; i < total_pages; i++)
    {
        first_page_table[i] = (i * 4096) | I86_PTE_PRESENT | I86_PTE_WRITABLE;
    }

    page_directory[0] = ((uint32_t)first_page_table) | I86_PDE_PRESENT | I86_PDE_WRITABLE;

    page_directory[768] = ((uint32_t)first_page_table) | I86_PDE_PRESENT | I86_PDE_WRITABLE;

    vmm_enable_paging();

    kprintf("VMM Dynamic Init:\n");
    kprintf(" - Identity Map Limit: 0x%x (%d Pages)\n", kernel_limit, total_pages);
}