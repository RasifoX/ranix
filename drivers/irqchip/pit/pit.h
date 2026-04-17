#ifndef PIT_H
#define PIT_H

#include <uapi/types.h>

void pit_init(uint32_t hz);
void pit_handler(void);
void pit_register(void);

#endif
