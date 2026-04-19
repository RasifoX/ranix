#ifndef PS2_H
#define PS2_H

#include <uapi/types.h>

void ps2_kbd_init(void);
int  ps2_kbd_has_data(void);
char ps2_kbd_getc(void);

#endif
