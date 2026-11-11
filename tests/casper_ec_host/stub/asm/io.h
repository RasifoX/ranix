/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* Port I/O for the host model: the EC's event ports live in model.c. */
#include <model.h>
uint8_t inb(uint16_t port);
void    outb(uint16_t port, uint8_t v);
