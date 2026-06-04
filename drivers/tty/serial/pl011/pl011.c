#include <drivers/serial/pl011.h>
#include <drivers/irqchip/irqchip.h>
#include <kernel/board.h>
#include <kernel/driver.h>
#include <kernel/irq.h>
#include <kernel/tty/tty_core.h>
#include <kernel/lib/string.h>

static uint64_t uart_base;

#define DR       (*(volatile unsigned int *)(uart_base + 0x00))
#define FR       (*(volatile unsigned int *)(uart_base + 0x18))
#define IBRD     (*(volatile unsigned int *)(uart_base + 0x24))
#define FBRD     (*(volatile unsigned int *)(uart_base + 0x28))
#define LCR_H    (*(volatile unsigned int *)(uart_base + 0x2C))
#define CR       (*(volatile unsigned int *)(uart_base + 0x30))
#define IMSC     (*(volatile unsigned int *)(uart_base + 0x38))
#define MIS      (*(volatile unsigned int *)(uart_base + 0x40))
#define ICR      (*(volatile unsigned int *)(uart_base + 0x44))
#define TXFF     (1 << 5)
#define RXFE     (1 << 4)
#define RXI      (1 << 4)   /* RX + RX-timeout interrupt (IMSC/MIS bit 4 / 6) */
#define RTI      (1 << 6)

/* QEMU virt PL011 SPI 1 → GIC INTID 33 */
#define PL011_IRQ  33

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

static void pl011_irq_handler(void)
{
    ICR = RXI | RTI;
    while (!(FR & RXFE))
        tty_input_char((char)DR);
}

void pl011_irq_init(void)
{
    if (!uart_base) return;
    ICR = 0x7FF;
    IMSC |= RXI | RTI;
    irq_register(PL011_IRQ, pl011_irq_handler);
    irqchip_enable_irq(PL011_IRQ);
}