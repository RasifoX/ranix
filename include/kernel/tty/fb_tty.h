#ifndef FB_TTY_H
#define FB_TTY_H

#include <kernel/fb.h>

void fb_tty_init(fb_info_t *fb);
void fb_tty_putchar(char c);
void fb_tty_puts(const char *s);
void fb_tty_flush(void);
void fb_tty_clear(void);

#endif
