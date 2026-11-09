/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
#ifndef IRQCHIP_H
#define IRQCHIP_H
#include <uapi/types.h>

void irqchip_init(uint64_t dist_base, uint64_t cpu_base);
void irqchip_enable_irq(uint32_t irq);
void irqchip_secondary_init(uint64_t cpu_base);   /* GIC CPU interface of a secondary CPU */

#endif
