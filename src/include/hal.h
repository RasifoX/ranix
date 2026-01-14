#ifndef HAL_H
#define HAL_H

#include <stdint.h>

void hal_init(void);

void hal_kputc(char c);
void hal_kprint(const char *str);

void hal_cpu_halt(void);
void hal_cpu_enable_interrupts(void);
void hal_cpu_disable_interrupts(void);

#endif