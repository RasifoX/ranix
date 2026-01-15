#ifndef ARCH_X86_SERIAL_H
#define ARCH_X86_SERIAL_H

#include <stdint.h>

#define COM1_PORT 0x3F8

int serial_init(void);

void serial_putc(char c);

void serial_print(const char *str);

void serial_clear_screen(void);

void serial_handler(void);

#endif