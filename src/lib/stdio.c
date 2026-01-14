#include "stdio.h"
#include "hal.h"

void itoa(char *buf, int base, int d)
{
    char *p = buf;
    char *p1, *p2;
    unsigned long ud = d;
    int divisor = 10;

    if (base == 10 && d < 0)
    {
        *p++ = '-';
        buf++;
        ud = -d;
    }
    else if (base == 16)
    {
        divisor = 16;
    }

    do
    {
        int remainder = ud % divisor;
        *p++ = (remainder < 10) ? remainder + '0' : remainder + 'a' - 10;
    } while (ud /= divisor);

    *p = 0;

    p1 = buf;
    p2 = p - 1;
    while (p1 < p2)
    {
        char tmp = *p1;
        *p1 = *p2;
        *p2 = tmp;
        p1++;
        p2--;
    }
}

void kprintf(const char *format, ...)
{
    char **arg = (char **)&format;
    int c;
    char buf[20];

    arg++;

    while ((c = *format++) != 0)
    {
        if (c != '%')
        {
            hal_kputc(c);
        }
        else
        {
            char *p, *p2;
            int pad0 = 0, pad = 0;

            c = *format++;
            if (c == '0')
            {
                pad0 = 1;
                c = *format++;
            }

            if (c >= '0' && c <= '9')
            {
                pad = c - '0';
                c = *format++;
            }

            switch (c)
            {
            case 'd':
            case 'u':
            case 'x':
                itoa(buf, c == 'x' ? 16 : 10, *((int *)arg++));
                p = buf;
                goto string;
                break;

            case 's':
                p = *arg++;
                if (!p)
                    p = "(null)";

            string:
                for (p2 = p; *p2; p2++)
                    ;
                for (; p2 < p + pad; p2++)
                    hal_kputc(pad0 ? '0' : ' ');
                while (*p)
                    hal_kputc(*p++);
                break;

            default:
                hal_kputc(*((int *)arg++));
                break;
            }
        }
    }
}