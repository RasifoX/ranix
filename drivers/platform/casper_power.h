/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* Casper Nirvana adapter, battery and lid (casper_power.c). */
#ifndef DRIVERS_PLATFORM_CASPER_POWER_H
#define DRIVERS_PLATFORM_CASPER_POWER_H

void casper_power_init(void);          /* after casper_ec_init */
void casper_power_event(int kind);     /* ECP_AC 1, ECP_BATTERY 2, ECP_LID 3 (ec_hotkey.h) */
void casper_power_lid_start(void);     /* EC events are on: read the lid once */
int  casper_power_lid(void);           /* -1 unknown, 0 closed, 1 open */

#endif
