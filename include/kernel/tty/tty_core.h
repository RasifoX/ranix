#ifndef TTY_CORE_H
#define TTY_CORE_H

#include <uapi/types.h>
#include <kernel/vfs.h>

void tty_core_init(void);
void tty_task_gone(pid_t pid);   /* a task exits: gives back a console it held */
void tty_input_char(char c);
void tty_input_str(const char *s);

#endif
