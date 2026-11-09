/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
#ifndef KERNEL_WATCHDOG_H
#define KERNEL_WATCHDOG_H

/* Unattended-boot safety net: hardware watchdog plus reset-on-panic.
 * See docs/watchdog_pstore.md. x86_64 only; the panic hooks compile away on
 * other architectures so kernel/kprintf.c stays shared. */

#if defined(__x86_64__)
void watchdog_init(void);        /* cmdline + hardware arm; early, before threads */
void watchdog_start(void);       /* health and pet threads; needs SMP up */
void watchdog_touch(void);       /* boot path: pet as proof of forward progress */
void watchdog_boot_done(void);   /* boot finished: the pet thread takes over */

void panic_reset_begin(const char *msg);  /* first thing in a panic, no locks */
void panic_reset_nested(void);            /* a fault while painting the panic */
void panic_reset_finish(void);            /* banner done: reset, or return to freeze */
int  panic_will_reset(void);
#else
static inline void panic_reset_begin(const char *msg) { (void)msg; }
static inline void panic_reset_nested(void) {}
static inline void panic_reset_finish(void) {}
static inline int  panic_will_reset(void) { return 0; }
#endif

/* Deadline watchdog (drivers/watchdog/wdog_core.c, x86_64 only; callers on
 * other architectures reach it through a weak reference). */
unsigned wdog_deadline_left(void);
int      wdog_deadline_set(unsigned secs);

#endif
