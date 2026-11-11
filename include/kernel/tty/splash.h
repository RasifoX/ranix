/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
#ifndef SPLASH_H
#define SPLASH_H

#include <kernel/fb.h>

/* The quiet boot and shutdown screen (kernel/tty/splash.c). */
int  splash_wanted(void);            /* "quiet", no ranix.verbose, no ranix.splash=0 */
void splash_init(fb_info_t *fb);     /* right after fb_tty_init: paints when wanted */
void splash_progress(int pct);       /* the kernel's own boot steps, 0-100 */
void splash_display_taken(void);     /* fb_tty: a program took the pixels */
void splash_display_back(void);      /* fb_tty: the program gave them back */
void splash_error_line(void);        /* kprintf: a KERN_ERR line went to the log */
void splash_key_esc(void);
void splash_panic(void);             /* kprintf: a panic owns the screen from here */           /* input: Esc on a real keyboard */
long splash_ctl_write(const char *w, size_t n);   /* /proc/sys/kernel/ranix_splash */
int  splash_ctl_read(char *buf, size_t cap);

#endif
