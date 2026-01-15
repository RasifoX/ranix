#include "serial.h"
#include "io.h"
#include "keyboard.h"

int serial_init(void)
{
    outb(COM1_PORT + 1, 0x00);

    outb(COM1_PORT + 3, 0x80);
    outb(COM1_PORT + 0, 0x03);
    outb(COM1_PORT + 1, 0x00);

    outb(COM1_PORT + 3, 0x03);

    outb(COM1_PORT + 2, 0x07);

    outb(COM1_PORT + 4, 0x0B);

    outb(COM1_PORT + 1, 0x01);

    return 0;
}

static int is_transmit_empty()
{
    return inb(COM1_PORT + 5) & 0x20;
}

static void serial_write_char(char c)
{
    while (is_transmit_empty() == 0)
        ;
    outb(COM1_PORT, c);
}

void serial_putc(char c)
{
    if (c == '\b')
    {
        serial_write_char('\b');
        serial_write_char(' ');
        serial_write_char('\b');
    }
    else
    {
        serial_write_char(c);
    }
}

void serial_print(const char *str)
{
    while (*str)
    {
        serial_putc(*str++);
    }
}

void serial_clear_screen(void)
{
    serial_print("\033[2J\033[H");
}

void serial_handler(void)
{
    while ((inb(COM1_PORT + 2) & 0x01) == 0)
    {
        if (inb(COM1_PORT + 5) & 0x01)
        {
            char c = inb(COM1_PORT);

            if (c == '\r')
                c = '\n';
            else if (c == 0x7F)
                c = '\b';

            keyboard_push_char(c);
        }
    }
}