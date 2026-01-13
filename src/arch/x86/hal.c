#include "hal.h"
#include "gdt.h"
#include "vga_text.h"

void hal_init() {
    init_gdt();
    terminal_initialize();
}

void hal_kprint(const char* str) {
    terminal_print(str);
}