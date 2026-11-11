/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* Casper Nirvana EC: the one access path (OS flags, shared RAM, events, pins). See casper_ec.c. */
#ifndef DRIVERS_PLATFORM_CASPER_EC_H
#define DRIVERS_PLATFORM_CASPER_EC_H

#include <uapi/types.h>

int  casper_ec_command(const char *cmd, size_t len);   /* "on", "off", "read": 0 or -errno */
int  casper_ec_status(char *buf, size_t cap);           /* the cached state as text, no EC access */
int  casper_ec_board(void);                              /* 1 on the board the DSDT is from */
/* The one way into this EC (all under its lock): */
int  casper_ec_ram1(uint8_t *buf, int len);              /* ECW1 shared RAM, read only: 0 or -errno */
int  casper_ec_event(void);                              /* one event query: number, or EC_NONE/BUSY/TIMEOUT */
int  casper_ec_pending(void);                            /* 1: an event waits (SCI_EVT) */
int  casper_ec_pin(uint8_t port, uint8_t pin);           /* RPIN: 0/1 or -errno */

/* drivers/input/ec_keys.c, the one EC event reader */
void ec_events_kick(void);                               /* SCI: the EC raised its GPE (irq context) */
void ec_events_use_sci(void);                            /* the SCI is on: ask on kicks, not on a timer */
void acpi_ec_gpe_rearm(void);                            /* arch: the EC's GPE on again (x86 acpi_events.c) */

#endif
