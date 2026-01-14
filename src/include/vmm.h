#ifndef VMM_H
#define VMM_H

#include <stdint.h>

#define PAGE_SIZE 4096

#define I86_PTE_PRESENT 0x01
#define I86_PTE_WRITABLE 0x02
#define I86_PTE_USER 0x04
#define I86_PTE_WRITETHROUGH 0x08
#define I86_PTE_NOT_CACHEABLE 0x10
#define I86_PTE_ACCESSED 0x20
#define I86_PTE_DIRTY 0x40
#define I86_PTE_PAT 0x80
#define I86_PTE_GLOBAL 0x100
#define I86_PTE_FRAME 0x7FFFF000

#define I86_PDE_PRESENT 0x01
#define I86_PDE_WRITABLE 0x02
#define I86_PDE_USER 0x04
#define I86_PDE_PWT 0x08
#define I86_PDE_PCD 0x10
#define I86_PDE_ACCESSED 0x20
#define I86_PDE_FRAME 0x7FFFF000

void vmm_init();
int vmm_map_page(void *phys, void *virt);
void vmm_enable_paging();

#endif