/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
#ifndef PIT_H
#define PIT_H

#include <uapi/types.h>

void pit_init(uint32_t hz);
void pit_handler(void);
void pit_register(void);

#endif
