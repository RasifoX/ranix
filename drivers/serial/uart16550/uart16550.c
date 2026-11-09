/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
#include "uart16550.h"
#include <drivers/serial/uart.h>
#include <drivers/irqchip/irqchip.h>
#include <asm/io.h>
#include <kernel/driver.h>
#include <kernel/irq.h>
#include <kernel/tty/tty_core.h>

#define COM1  0x3F8

#define THR   (COM1 + 0)
#define RBR   (COM1 + 0)
#define IER   (COM1 + 1)
#define FCR   (COM1 + 2)
#define LCR   (COM1 + 3)
#define MCR   (COM1 + 4)
#define LSR   (COM1 + 5)

#define LSR_TX_EMPTY  0x20
#define LSR_RX_READY  0x01

#define COM1_VEC  0x24   /* PIC IRQ4 → vector 0x24 */

void uart_init(void)
{
	uart16550_init();
}

void uart_putc(char c)     { uart16550_putc(c); }
char uart_getc(void)       { return uart16550_getc(); }
int  uart_has_data(void)   { return uart16550_has_data(); }

/* A machine with no COM1 floats the port to 0xFF, which reads as both "TX empty"
 * and "RX ready": every RX drain loop would then spin forever on phantom bytes.
 * Most laptops are this machine. Probed once; all paths go quiet if absent. */
static int uart_absent;

#define TX_WAIT_POLLS 100000   /* far above one 87 us character time */

void uart16550_init(void)
{
	if (inb(LSR) == 0xFF) {
		uart_absent = 1;
		return;
	}
	outb(IER, 0x00);          /* disable interrupts */
	outb(LCR, 0x80);          /* enable DLAB */
	outb(RBR, 0x01);          /* baud divisor low: 115200 */
	outb(IER, 0x00);          /* baud divisor high */
	outb(LCR, 0x03);          /* 8N1, clear DLAB */
	outb(FCR, 0xC7);          /* enable + clear FIFO, 14-byte threshold */
	outb(MCR, 0x03);          /* DTR + RTS */
}

void uart16550_putc(char c)
{
	if (uart_absent) return;
	for (int i = 0; !(inb(LSR) & LSR_TX_EMPTY); i++) {
		if (i == TX_WAIT_POLLS) {   /* stuck transmitter: never block the console */
			uart_absent = 1;
			return;
		}
	}
	outb(THR, (uint8_t)c);
}

int uart16550_has_data(void)
{
	if (uart_absent) return 0;
	return inb(LSR) & LSR_RX_READY;
}

char uart16550_getc(void)
{
	while (!uart16550_has_data());
	return (char)inb(RBR);
}

static int drv_read(void *buf, size_t len)
{
	char *p = buf;
	size_t i;
	for (i = 0; i < len && uart16550_has_data(); i++)
		p[i] = uart16550_getc();
	return (int)i;
}

static int drv_write(const void *buf, size_t len)
{
	const char *p = buf;
	for (size_t i = 0; i < len; i++)
		uart16550_putc(p[i]);
	return (int)len;
}

static driver_t uart16550_driver = {
	.name        = "uart16550",
	.type        = DRIVER_TYPE_SERIAL,
	.init        = 0,
	.read        = drv_read,
	.write       = drv_write,
	.ioctl       = 0,
	.initialized = 1,
};

void uart16550_register(void)
{
	driver_register(&uart16550_driver);
}

static void uart16550_irq_handler(void)
{
	while (inb(LSR) & LSR_RX_READY)
		tty_input_char((char)inb(RBR));
}

void uart16550_irq_init(void)
{
	if (uart_absent) return;
	outb(MCR, 0x0B);          /* DTR + RTS + OUT2 (gate IRQ to PIC) */
	outb(IER, 0x01);          /* enable received-data-available interrupt */
	irq_register(COM1_VEC, uart16550_irq_handler);
	irqchip_enable_irq(COM1_VEC);
}
