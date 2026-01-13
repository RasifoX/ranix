#include "hal.h"

void kernel_main()
{
    hal_init();

    hal_kprint("HAL initialized successfully!\n");
    hal_kprint("IDT initialized successfully!\n");
    hal_kprint("GDT initialized successfully!\n");
    hal_kprint("Terminal initialized successfully!\n");
    hal_kprint("PIC remapped successfully!\n");
    hal_kprint("\nHello from Ranix!");

    while (1);
}