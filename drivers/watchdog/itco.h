/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
#ifndef DRIVERS_WATCHDOG_ITCO_H
#define DRIVERS_WATCHDOG_ITCO_H

#include <uapi/types.h>

#define ITCO_TICKS_MIN 2
#define ITCO_TICKS_MAX 0x3FF

/* Hardware layer only: register discovery and programming. Policy (when to pet)
 * lives in wdog_core.c. Every function is bounded: no polling loops. */
int      itco_probe(void);               /* 0 = found and able to reset the machine */
int      itco_start(unsigned ticks);     /* 0 = counting down */
void     itco_pet(void);
void     itco_set_ticks(unsigned ticks); /* reprogram a running timer and reload */
uint16_t itco_counter(void);             /* ticks left in the current stage */
int      itco_fired_last_boot(void);     /* valid after itco_probe, before itco_start */

#endif
