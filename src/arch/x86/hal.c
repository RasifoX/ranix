#include <stdint.h>
#include "hal.h"
#include "gdt.h"
#include "idt.h"
#include "pic.h"
#include "keyboard.h"
#include "keymap.h"
#include "vga_text.h"
#include "pit.h"
#include "serial.h"

extern keymap_t keymap_us;

void hal_init()
{
    init_gdt();
    init_idt();
    serial_init();
    terminal_initialize();
    pic_remap(0x20, 0x28);

    init_pit(100);

    keyboard_init();
    keyboard_set_map(&keymap_us);
    
    pic_clear_mask(1);
    pic_clear_mask(4);
}

void hal_kputc(char c)
{
    terminal_putchar(c);

    serial_putc(c);
}

void hal_kprint(const char *str)
{
    terminal_print(str);

    serial_print(str);
}

void hal_clear_screen(void)
{
    terminal_clear();

    serial_clear_screen();
}

void hal_cpu_halt(void)
{
    asm volatile("hlt");
}

void hal_cpu_enable_interrupts(void)
{
    asm volatile("sti");
}

void hal_cpu_disable_interrupts(void)
{
    asm volatile("cli");
}

uint32_t hal_get_fault_addr(void)
{
    uint32_t val;
    asm volatile("mov %%cr2, %0" : "=r"(val));
    return val;
}