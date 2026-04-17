#include "uart16550.h"
#include <drivers/serial/uart.h>
#include <asm/io.h>
#include <kernel/driver.h>

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

void uart_init(void)
{
	uart16550_init();
}

void uart_putc(char c)     { uart16550_putc(c); }
char uart_getc(void)       { return uart16550_getc(); }
int  uart_has_data(void)   { return uart16550_has_data(); }

void uart16550_init(void)
{
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
	while (!(inb(LSR) & LSR_TX_EMPTY));
	outb(THR, (uint8_t)c);
}

int uart16550_has_data(void)
{
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
