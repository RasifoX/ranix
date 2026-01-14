#ifndef ARCH_X86_VGA_TEXT_H
#define ARCH_X86_VGA_TEXT_H

#include <stdint.h>
#include <stddef.h>

void terminal_initialize(void);
void terminal_putchar(char c);
void terminal_write(const char *data, size_t size);
void terminal_print(const char *data);

void terminal_clear(void);
void terminal_set_color(uint8_t color);
void terminal_enable_cursor(uint8_t cursor_start, uint8_t cursor_end);
void terminal_update_cursor(int x, int y);

#endif