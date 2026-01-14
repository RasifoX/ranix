#include <stdint.h>
#include "hal.h"
#include "multiboot.h"
#include "stdio.h"
#include "pmm.h"
#include "vmm.h"

void kernel_main(uint32_t magic, uint32_t addr)
{
    hal_init();

    kprintf("HAL initialized successfully.\n");
    kprintf("Hello from Ranix v0.0.1!\n");

    hal_cpu_enable_interrupts();
    kprintf("CPU Interrupts enabled.\n");

    if (magic != MULTIBOOT_BOOTLOADER_MAGIC)
    {
        kprintf("FATAL: Invalid bootloader magic number: 0x%x\n", magic);
        kprintf("Expected: 0x%x\n", MULTIBOOT_BOOTLOADER_MAGIC);
        // İleride panic() fonksiyonu buraya gelecek.
        return;
    }

    multiboot_info_t *mboot_ptr = (multiboot_info_t *)((uintptr_t)addr);

    if (mboot_ptr->flags & 1)
    {
        kprintf("RAM Information:\n");
        kprintf(" - Lower Memory: %d KB\n", mboot_ptr->mem_lower);
        kprintf(" - Upper Memory: %d KB\n", mboot_ptr->mem_upper);

        uint32_t total_mb = (mboot_ptr->mem_upper + 1024) / 1024;
        kprintf(" - Total Detected RAM: %d MB\n", total_mb);

        pmm_init(mboot_ptr->mem_upper);

        void *new_page = pmm_alloc_block();
        kprintf("Test Alloc: 0x%x\n", new_page);

        vmm_init();
    }
    else
    {
        kprintf("WARNING: Bootloader did not provide memory info!\n");
    }

    kprintf("\nSystem initialization complete. Entering idle loop.\n");

    while (1)
    {
        hal_cpu_halt();
    }
}