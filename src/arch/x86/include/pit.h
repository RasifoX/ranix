#ifndef ARCH_X86_PIT_H
#define ARCH_X86_PIT_H

#include <stdint.h>

void init_pit(uint32_t frequency);
void timer_handler();
void sleep(uint32_t ms);

#endif