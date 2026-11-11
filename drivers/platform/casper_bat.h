/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* Casper Nirvana battery and adapter from EC shared RAM (casper_bat.c: pure decode;
 * casper_power.c: reading it, the supplies, the lid). */
#ifndef DRIVERS_PLATFORM_CASPER_BAT_H
#define DRIVERS_PLATFORM_CASPER_BAT_H
#include <kernel/power_supply.h>
#ifdef HOST_TEST
#include <string.h>
#else
#include <kernel/lib/string.h>
#endif

#define CASPER_ECW1_LEN 0x60                     /* the bytes the decode reads */
int casper_ac_decode(const uint8_t *w, ps_t *ac);    /* 0, -1 when the bytes make no sense */
int casper_bat_decode(const uint8_t *w, ps_t *bat);

#endif
