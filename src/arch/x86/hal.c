#include <stdint.h>
#include "hal.h"
#include "gdt.h"
#include "idt.h"
#include "pic.h"
#include "keymap.h"
#include "vga_text.h"
#include "pit.h"

extern keymap_t keymap_us;

void hal_init()
{
    init_gdt();
    init_idt();
    terminal_initialize();
    pic_remap(0x20, 0x28);
    
    init_pit(100);

    keyboard_set_map(&keymap_us);

    asm volatile("sti");
}

void hal_kprint(const char *str)
{
    terminal_print(str);
}