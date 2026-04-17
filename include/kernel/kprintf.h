#ifndef KPRINTF_H
#define KPRINTF_H

void kprintf(const char *fmt, ...);
void kpanic(const char *msg);
void kprintf_set_fb(void);
void kprintf_flush(void);

#endif
