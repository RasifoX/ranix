#ifndef PS2_H
#define PS2_H

#include <uapi/types.h>

void ps2_kbd_init(void);
/* USB HID reports enter the keyboard and mouse path here (ps2.c) */
void input_usb_key(uint16_t code, int down);
void input_usb_mouse(int dx, int dy, int wheel, int buttons);
int  ps2_aux_report(char *buf, int len);          /* touchpad state for /sys */

#endif
