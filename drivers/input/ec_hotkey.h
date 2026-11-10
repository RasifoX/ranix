/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* Hotkeys that come as embedded-controller events (ec_hotkey.c: the protocol and
 * the per-board tables, no hardware; ec_keys.c: the polling and the keys). */
#ifndef DRIVERS_INPUT_EC_HOTKEY_H
#define DRIVERS_INPUT_EC_HOTKEY_H
#if __has_include(<uapi/types.h>)
#include <uapi/types.h>
#else
#include <stdint.h>
#endif

#define EC_DATA    0x62
#define EC_SC      0x66            /* status on read, command on write */
#define EC_OBF     0x01            /* the EC has a byte for us */
#define EC_IBF     0x02            /* the EC has not taken our last byte yet */
#define EC_SCI_EVT 0x20            /* an event is waiting */
#define EC_QR      0x84            /* "which event?" */

typedef struct {
	uint8_t (*in)(uint16_t port);
	void    (*out)(uint16_t port, uint8_t v);
	void    (*wait)(void);           /* about 10 microseconds */
} ec_io_t;

enum { EC_NONE = -1, EC_BUSY = -2, EC_TIMEOUT = -3 };
/* One event: its number (1-255), or EC_NONE / EC_BUSY / EC_TIMEOUT. */
int ec_query(const ec_io_t *io);

enum { ECA_NONE, ECA_MUTE, ECA_VOL_DOWN, ECA_VOL_UP, ECA_BRIGHT_DOWN, ECA_BRIGHT_UP };
/* What the Casper Nirvana's AML does for event q, as an action. */
int ec_hotkey_casper(int q);
#endif
