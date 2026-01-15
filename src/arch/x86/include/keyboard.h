#ifndef ARCH_X86_KEYBOARD_H
#define ARCH_X86_KEYBOARD_H

#include <stdint.h>

#define KB_BUFFER_SIZE 256

void keyboard_init(void);

void keyboard_handler(void);

char keyboard_get_char(void);

int keyboard_has_chars(void);

void keyboard_push_char(char c);

#endif