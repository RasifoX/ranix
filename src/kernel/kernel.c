#include <stdint.h>
#include "hal.h"
#include "multiboot.h"
#include "stdio.h"
#include "pmm.h"
#include "vmm.h"
#include "heap.h"
#include "shell.h"

extern uint32_t _kernel_end;

void kernel_main(uint32_t magic, uint32_t addr)
{
    hal_init();
    hal_clear_screen(); 

    kprintf("Ranix Kernel v0.0.1 Booting...\n");
    kprintf("----------------------------\n");

    hal_cpu_enable_interrupts();
    kprintf("[+] Interrupts enabled\n");

    if (magic != MULTIBOOT_BOOTLOADER_MAGIC)
    {
        kprintf("FATAL: Invalid bootloader magic: 0x%x\n", magic);
        return;
    }

    multiboot_info_t *mboot_ptr = (multiboot_info_t *)((uintptr_t)addr);

    if (mboot_ptr->flags & 1)
    {
        uint32_t total_mb = (mboot_ptr->mem_upper + 1024) / 1024;
        kprintf("[+] Detected RAM: %d MB\n", total_mb);

        pmm_init(mboot_ptr->mem_upper);
 
        vmm_init();
        
        uint32_t mem_total_kb = mboot_ptr->mem_upper + 1024;
        uint32_t max_blocks = mem_total_kb / 4;
        uint32_t bitmap_size = max_blocks / 8;
        
        uint32_t heap_start = ((uint32_t)&_kernel_end) + bitmap_size + 4096;
        heap_start = (heap_start + 4095) & ~4095;

        kheap_init((void*)heap_start, 1024 * 1024);
        kprintf("[+] Kernel Heap Initialized (1MB)\n");
    }
    else
    {
        kprintf("WARNING: Bootloader did not provide memory info!\n");
    }

    kprintf("----------------------------\n");
    kprintf("System Ready. Type something...\n");

    shell_init();

    while (1)
    {
        shell_update();
        
        hal_cpu_halt();
    }
}