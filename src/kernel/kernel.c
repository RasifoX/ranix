#include <stdint.h>
#include "hal.h"
#include "multiboot.h"
#include "stdio.h"
#include "pmm.h"
#include "vmm.h"
#include "heap.h"
#include "shell.h"
#include "process.h"
#include "fs.h"
#include "initrd.h"

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

        kheap_init((void *)heap_start, 1024 * 1024);
        kprintf("[+] Kernel Heap Initialized (1MB)\n");
    }
    else
    {
        kprintf("WARNING: Bootloader did not provide memory info!\n");
    }

    if (mboot_ptr->flags & 0x08)
    {
        kprintf("Modules found: %d\n", mboot_ptr->mods_count);

        // Modül bulunduktan sonra:
        if (mboot_ptr->mods_count > 0)
        {
            multiboot_module_t *mod = (multiboot_module_t *)((uintptr_t)mboot_ptr->mods_addr);

            // Initrd Sürücüsünü Başlat
            // Bu bize bir "Root Node" döndürecek.
            fs_root = initialise_initrd(mod->mod_start, mod->mod_end);

            kprintf("[+] VFS Initialized. Root mounted.\n");

            // TEST: VFS Üzerinden Dosya Okuma
            // 1. Dosyayı Bul
            fs_node_t *fsnode = finddir_fs(fs_root, "test.txt");

            if (fsnode)
            {
                kprintf("Found file: %s (Size: %d bytes)\n", fsnode->name, fsnode->length);

                // 2. İçeriğini Oku
                char buf[256];
                uint32_t sz = read_fs(fsnode, 0, 255, (uint8_t *)buf);
                buf[sz] = 0; // Null terminator

                kprintf("File Content (via VFS): %s\n", buf);
            }
            else
            {
                kprintf("Error: Could not find test.txt in VFS!\n");
            }
        }
    }
    else
    {
        kprintf("No modules loaded via GRUB.\n");
    }

    kprintf("----------------------------\n");

    scheduler_init();

    shell_init();

    while (1)
    {
        shell_update();

        hal_cpu_halt();
    }
}