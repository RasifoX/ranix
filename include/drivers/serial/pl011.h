/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
#ifndef PL011_H
#define PL011_H

#include <drivers/serial/uart.h>

void pl011_register(void);
void pl011_irq_init(void);

#endif
