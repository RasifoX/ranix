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
#include "gdt.h"

extern uint32_t _kernel_end;
extern uint32_t stack_top;
extern void enter_user_mode(uint32_t entry_point);
extern void shell_main();

static inline int syscall(uint32_t num, uint32_t a1, uint32_t a2, uint32_t a3)
{
    register uint32_t eax asm("eax") = num;
    register uint32_t ebx asm("ebx") = a1;
    register uint32_t ecx asm("ecx") = a2;
    register uint32_t edx asm("edx") = a3;

    asm volatile("int $0x80"
                 : "+r"(eax)
                 : "r"(ebx), "r"(ecx), "r"(edx)
                 : "memory");
    return eax;
}

void user_mode_entry()
{
    char *msg = "\n[User] Keyboard Test. Type something (Press 'q' to exit)...\n";

    syscall(4, 1, (uint32_t)msg, 60);

    char buf[2];
    buf[1] = 0;

    while (1)
    {
        int n = syscall(3, 0, (uint32_t)buf, 1);

        if (n > 0)
        {
            syscall(4, 1, (uint32_t)buf, 1);

            if (buf[0] == 'q')
                break;
        }
    }

    syscall(1, 0, 0, 0);

    while (1)
        ;
}

void user_mode_wrapper()
{
    kprintf("[Kernel] Starting User Shell...\n");
    enter_user_mode((uint32_t)shell_main);
}

void kernel_main(uint32_t magic, uint32_t addr)
{
    hal_init();
    kprintf("Ranix Kernel (Multiboot2) Booting...\n");
    kprintf("------------------------------------\n");

    if (magic != MULTIBOOT2_BOOTLOADER_MAGIC)
    {
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

    pmm_init(mem_upper);
    vmm_init();

    uint32_t mem_total_kb = mem_upper + 1024;
    uint32_t max_blocks = mem_total_kb / 4;
    uint32_t bitmap_size = max_blocks / 8;

    uint32_t heap_start = ((uint32_t)&_kernel_end) + bitmap_size + 4096;
    heap_start = (heap_start + 4095) & ~4095;

    kheap_init((void *)heap_start, 1024 * 1024);

    if (mod_start > 0)
        fs_root = initialise_initrd(mod_start, mod_end);

    scheduler_init();

    tss_set_stack(0x10, (uint32_t)&stack_top);

    kprintf("[+] Multitasking Enabled.\n");
    kprintf("------------------------------------\n");
    scheduler_add_task(user_mode_wrapper, "UserShell");

    // shell_init();

    while (1)
    {
        // shell_update();
        hal_cpu_halt();
    }
}