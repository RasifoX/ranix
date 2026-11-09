/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
#ifndef UART_H
#define UART_H

void uart_init(void);
void uart_putc(char c);
char uart_getc(void);
int  uart_has_data(void);

#endif
