#ifndef TTY_CORE_H
#define TTY_CORE_H

#include <uapi/types.h>
#include <kernel/vfs.h>

void tty_core_init(void);
void tty_input_char(char c);

#endif
