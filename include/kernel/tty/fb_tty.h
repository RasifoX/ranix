/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
#ifndef FB_TTY_H
#define FB_TTY_H

#include <kernel/fb.h>

void fb_tty_init(fb_info_t *fb);
void fb_tty_putchar(char c);
void fb_tty_puts(const char *s);
void fb_tty_write(const char *buf, size_t len);
void fb_tty_console_write(const char *buf, size_t len);   /* programs' console output: kept for a reveal */
size_t fb_tty_console_tail(char *out, size_t cap);
void fb_tty_flush(void);
void fb_tty_set_graphics(int on);
int  fb_tty_get_graphics(void);
void fb_tty_clear(void);
void fb_tty_show_cursor(int show);
void fb_tty_set_col(u32 col);
void fb_tty_erase_eol(void);
void fb_tty_get_size(int *cols, int *rows);
void fb_tty_get_cursor(int *col, int *row);
void fb_tty_get_font(int *w, int *h);     /* one cell in pixels */
void fb_tty_clear_shadow(void);           /* blank text, screen untouched while covered */
void fb_tty_set_cover(int on);            /* the boot screen owns the pixels (splash.c) */
int  fb_tty_get_cover(void);
const u8 *fb_tty_glyph(u32 cp);           /* 8x16 bitmap, one byte a row */

#endif
