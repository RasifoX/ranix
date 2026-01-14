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
}

void hal_kputc(char c)
{
    terminal_putchar(c);
}

void hal_kprint(const char *str)
{
    terminal_print(str);
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