#include "hal.h"

void kernel_main()
{
    hal_init();

    hal_kprint("Terminal initialized successfully!\n");
    hal_kprint("HAL initialized successfully!\n");
    hal_kprint("IDT initialized successfully!\n");
    hal_kprint("GDT initialized successfully!\n");
    hal_kprint("Hello from Ranix!");
    volatile int a = 1 / 0;
}