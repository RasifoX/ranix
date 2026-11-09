/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
#ifndef PIC8259_H
#define PIC8259_H

#include <uapi/types.h>

void pic8259_init(void);
void pic8259_eoi(uint32_t irq);
void pic8259_mask(uint32_t irq);
void pic8259_unmask(uint32_t irq);

#endif
