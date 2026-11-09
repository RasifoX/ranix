#ifndef TIMER_H
#define TIMER_H
#include <uapi/types.h>

void timer_handler(void);
void timer_init(uint32_t hz);
uint64_t timer_ticks(void);
void timer_register(void);
void timer_secondary_init(void);                  /* per-CPU timer of a secondary CPU */

#endif
