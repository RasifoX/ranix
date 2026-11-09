/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* Casper Nirvana EC: the "OS present" flags its AML writes at boot. See casper_ec.c. */
#ifndef DRIVERS_PLATFORM_CASPER_EC_H
#define DRIVERS_PLATFORM_CASPER_EC_H

#include <uapi/types.h>

int  casper_ec_command(const char *cmd, size_t len);   /* "on", "off", "read": 0 or -errno */
int  casper_ec_status(char *buf, size_t cap);           /* the cached state as text, no EC access */

#endif
