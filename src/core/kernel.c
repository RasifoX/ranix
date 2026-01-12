#include "gdt.h"
#include "vga_text.h"

void kernel_main()
{
    init_gdt();

    terminal_initialize();

    terminal_print("GDT initialized successfully!\n");
    terminal_print("Hello from Ranix!");
}