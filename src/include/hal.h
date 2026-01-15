#ifndef HAL_H
#define HAL_H

#include <stdint.h>

void hal_init(void);

void hal_kputc(char c);
void hal_kprint(const char *str);
void hal_clear_screen(void);

void hal_cpu_halt(void);
void hal_cpu_enable_interrupts(void);
void hal_cpu_disable_interrupts(void);

uint32_t hal_get_fault_addr(void);

void enter_user_mode(uint32_t func_addr);

#endif