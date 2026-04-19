#include <drivers/serial/uart.h>
#include <kernel/hal.h>
#include <kernel/irq.h>
#include <kernel/kprintf.h>
#include <kernel/tty/fb_tty.h>

typedef __builtin_va_list va_list;
#define va_start(v, l) __builtin_va_start(v, l)
#define va_end(v)      __builtin_va_end(v)
#define va_arg(v, l)   __builtin_va_arg(v, l)

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

static void fmt_uint(uint64_t n, int base, int width, char pad, int upper)
{
    char buf[64];
    int  i = 0;
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";

    if (n == 0)
        buf[i++] = '0';
    else
        while (n) { buf[i++] = digits[n % (unsigned)base]; n /= (unsigned)base; }

    while (i < width)
        buf[i++] = pad;

    while (i--)
        _putc(buf[i]);
}

void kprintf(const char *fmt, ...)
{
    uint64_t flags = irq_save();
    va_list args;
    va_start(args, fmt);

    while (*fmt) {
        if (*fmt != '%') { _putc(*fmt++); continue; }
        fmt++;

        char pad = ' ';
        if (*fmt == '0') { pad = '0'; fmt++; }

        int width = 0;
        while (*fmt >= '0' && *fmt <= '9')
            width = width * 10 + (*fmt++ - '0');

        int lng = 0;
        while (*fmt == 'l') { lng++; fmt++; }
        if (*fmt == 'z') { lng = 1; fmt++; }

        switch (*fmt) {
        case 'd': case 'i': {
            int64_t v = lng ? (int64_t)va_arg(args, long) : (int64_t)va_arg(args, int);
            if (v < 0) { _putc('-'); fmt_uint((uint64_t)-v, 10, width, pad, 0); }
            else        fmt_uint((uint64_t)v,  10, width, pad, 0);
            break;
        }
        case 'u':
            fmt_uint(lng ? va_arg(args, unsigned long) : (uint64_t)va_arg(args, unsigned int),
                     10, width, pad, 0);
            break;
        case 'x':
            fmt_uint(lng ? va_arg(args, unsigned long) : (uint64_t)va_arg(args, unsigned int),
                     16, width, pad, 0);
            break;
        case 'X':
            fmt_uint(lng ? va_arg(args, unsigned long) : (uint64_t)va_arg(args, unsigned int),
                     16, width, pad, 1);
            break;
        case 'p': {
            uint64_t v = va_arg(args, unsigned long);
            _puts("0x");
            fmt_uint(v, 16, 16, '0', 0);
            break;
        }
        case 's': {
            const char *s = va_arg(args, const char *);
            _puts(s ? s : "(null)");
            break;
        }
        case 'c':
            _putc((char)va_arg(args, int));
            break;
        case '%':
            _putc('%');
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
