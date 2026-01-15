#include <stdint.h>
#include "hal.h"
#include "multiboot2.h"
#include "stdio.h"
#include "pmm.h"
#include "vmm.h"
#include "heap.h"
#include "shell.h"
#include "process.h"
#include "fs.h"
#include "initrd.h"

extern uint32_t _kernel_end;
extern uint32_t stack_top;

void kernel_main(uint32_t magic, uint32_t addr)
{
    hal_init();

    kprintf("Ranix Kernel (Multiboot2) Booting...\n");
    kprintf("------------------------------------\n");

    if (magic != MULTIBOOT2_BOOTLOADER_MAGIC)
    {
        kprintf("FATAL: Invalid magic number: 0x%x\n", magic);
        return;
    }

    struct multiboot_tag *tag;
    uint32_t mem_upper = 0;
    uint32_t mod_start = 0;
    uint32_t mod_end = 0;

    for (tag = (struct multiboot_tag *)(addr + 8);
         tag->type != MULTIBOOT_TAG_TYPE_END;
         tag = (struct multiboot_tag *)((uint8_t *)tag + ((tag->size + 7) & ~7)))
    {
        if (tag->type == MULTIBOOT_TAG_TYPE_BASIC_MEMINFO)
        {
            mem_upper = ((struct multiboot_tag_basic_meminfo *)tag)->mem_upper;
        }
        else if (tag->type == MULTIBOOT_TAG_TYPE_MODULE)
        {
            mod_start = ((struct multiboot_tag_module *)tag)->mod_start;
            mod_end = ((struct multiboot_tag_module *)tag)->mod_end;
        }
    }

    if (mem_upper == 0)
    {
        kprintf("FATAL: Memory info missing!\n");
        while (1)
            hal_cpu_halt();
    }

    pmm_init(mem_upper);
    vmm_init();

    uint32_t mem_total_kb = mem_upper + 1024;
    uint32_t max_blocks = mem_total_kb / 4;
    uint32_t bitmap_size = max_blocks / 8;

    uint32_t heap_start = ((uint32_t)&_kernel_end) + bitmap_size + 4096;
    heap_start = (heap_start + 4095) & ~4095;

    kheap_init((void *)heap_start, 1024 * 1024);
    kprintf("[+] Memory System: PMM, VMM and Heap Ready.\n");

    if (mod_start > 0)
    {
        fs_root = initialise_initrd(mod_start, mod_end);
        kprintf("[+] VFS: Initrd mounted.\n");
    }
    else
    {
        kprintf("[-] Warning: No Initrd module found.\n");
    }

    kprintf("------------------------------------\n");

    scheduler_init();

    tss_set_stack(0x10, (uint32_t)&stack_top);

    hal_cpu_enable_interrupts();
    kprintf("[+] System Running.\n");

    shell_init();

    while (1)
    {
        shell_update();
        hal_cpu_halt();
    }
}