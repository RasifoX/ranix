#include "hal.h"
#include "gdt.h"
#include "idt.h"
#include "vga_text.h"

void hal_init()
{
    init_gdt();
    init_idt();
    terminal_initialize();
}

void hal_kprint(const char *str)
{
    terminal_print(str);
}