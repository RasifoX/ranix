#include <drivers/serial/uart.h>
#include <kernel/hal.h>
#include <kernel/irq.h>
#include <kernel/kprintf.h>
#include <kernel/tty/fb_tty.h>

typedef __builtin_va_list va_list;
#define va_start(v, l) __builtin_va_start(v, l)
#define va_end(v) __builtin_va_end(v)
#define va_arg(v, l) __builtin_va_arg(v, l)

static int fb_ready = 0;

void kprintf_set_fb(void) { fb_ready = 1; }

static void _putc(char c)
{
    if (c == '\n')
        uart_putc('\r');
    uart_putc(c);
    if (fb_ready)
        fb_tty_putchar(c);
}

static void _puts(const char *s)
{
    while (*s)
        _putc(*s++);
}

static void print_uint(unsigned int n, int base)
{
    char buf[32];
    int i = 0;
    if (n == 0)
    {
        _putc('0');
        return;
    }
    while (n)
    {
        int r = n % base;
        buf[i++] = r < 10 ? '0' + r : 'a' + r - 10;
        n /= base;
    }
    while (i--)
        _putc(buf[i]);
}

static void print_uint_padded(unsigned int n, int base, int width)
{
    char buf[32];
    int i = 0;
    if (n == 0 && width == 0) { _putc('0'); return; }
    while (n) {
        int r = n % base;
        buf[i++] = r < 10 ? '0' + r : 'a' + r - 10;
        n /= base;
    }
    /* zero-pad to width */
    while (i < width) buf[i++] = '0';
    while (i--) _putc(buf[i]);
}

void kprintf(const char *fmt, ...)
{
    uint64_t flags = irq_save();

    va_list args;
    va_start(args, fmt);
    while (*fmt)
    {
        if (*fmt != '%')
        {
            _putc(*fmt++);
            continue;
        }
        fmt++;
        switch (*fmt)
        {
        case 'd':
        {
            int v = va_arg(args, int);
            if (v < 0)
            {
                _putc('-');
                print_uint((unsigned int)(-v), 10);
            }
            else
                print_uint((unsigned int)v, 10);
            break;
        }
        case 'x':
        {
            unsigned int v = va_arg(args, unsigned int);
            _puts("0x");
            print_uint(v, 16);
            break;
        }
        case 'l':
        {
            fmt++;
            if (*fmt == 'x') {
                unsigned long v = va_arg(args, unsigned long);
                /* no "0x" here — caller adds it in the format string */
                unsigned int hi = (unsigned int)(v >> 32);
                unsigned int lo = (unsigned int)(v & 0xFFFFFFFF);
                if (hi) {
                    print_uint_padded(hi, 16, 0);   /* upper, no padding */
                    print_uint_padded(lo, 16, 8);   /* lower, always 8 digits */
                } else {
                    print_uint_padded(lo, 16, 0);   /* fits in 32 bits */
                }
            }
            break;
        }
        case 'p':
        {
            unsigned long v = va_arg(args, unsigned long);
            _puts("0x");
            unsigned int hi = (unsigned int)(v >> 32);
            unsigned int lo = (unsigned int)(v & 0xFFFFFFFF);
            if (hi) {
                print_uint_padded(hi, 16, 0);
                print_uint_padded(lo, 16, 8);
            } else {
                print_uint_padded(lo, 16, 0);
            }
            break;
        }
        case 's':
            _puts(va_arg(args, const char *));
            break;
        case 'c':
            _putc((char)va_arg(args, int));
            break;
        default:
            _putc('%');
            _putc(*fmt);
            break;
        }
        fmt++;
    }
    va_end(args);

    irq_restore(flags);
}

void kprintf_flush(void)
{
    if (fb_ready)
        fb_tty_flush();
}

void kpanic(const char *msg)
{
    irq_disable();
    _puts("\n[PANIC] ");
    _puts(msg);
    _puts("\n--- halted ---\n");
    for (;;)
        arch_halt();
}
