#include <drivers/serial/pl011.h>
#include <kernel/board.h>
#include <kernel/driver.h>
#include <kernel/lib/string.h>

static uint64_t uart_base;

#define DR       (*(volatile unsigned int *)(uart_base + 0x00))
#define FR       (*(volatile unsigned int *)(uart_base + 0x18))
#define IBRD     (*(volatile unsigned int *)(uart_base + 0x24))
#define FBRD     (*(volatile unsigned int *)(uart_base + 0x28))
#define LCR_H    (*(volatile unsigned int *)(uart_base + 0x2C))
#define CR       (*(volatile unsigned int *)(uart_base + 0x30))
#define TXFF     (1 << 5)
#define RXFE     (1 << 4)

void uart_init(void) {
    uart_base = board_get_config()->uart_base;
}

void uart_putc(char c) {
    while (FR & TXFF);
    DR = c;
}

void uart_puts(const char *s) {
    while (*s) {
        if (*s == '\n') uart_putc('\r');
        uart_putc(*s++);
    }
}

int uart_has_data(void) {
    return !(FR & RXFE);
}

char uart_getc(void) {
    while (FR & RXFE);
    return (char)DR;
}

static int pl011_drv_read(void *buf, size_t len)
{
    char *p = (char *)buf;
    size_t i;
    for (i = 0; i < len; i++) {
        if (FR & RXFE) break;
        p[i] = (char)DR;
    }
    return (int)i;
}

static int pl011_drv_write(const void *buf, size_t len)
{
    const char *p = (const char *)buf;
    for (size_t i = 0; i < len; i++)
        uart_putc(p[i]);
    return (int)len;
}

static driver_t pl011_driver = {
    .name = "pl011",
    .type = DRIVER_TYPE_SERIAL,
    .init = 0,
    .read = pl011_drv_read,
    .write = pl011_drv_write,
    .ioctl = 0,
    .initialized = 1,
};

void pl011_register(void)
{
    driver_register(&pl011_driver);
}