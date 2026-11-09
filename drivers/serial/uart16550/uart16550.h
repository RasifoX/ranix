/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
#ifndef UART16550_H
#define UART16550_H

void uart16550_init(void);
void uart16550_putc(char c);
char uart16550_getc(void);
int  uart16550_has_data(void);
void uart16550_register(void);
void uart16550_irq_init(void);

#endif
