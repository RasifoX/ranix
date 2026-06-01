#ifndef KPRINTF_H
#define KPRINTF_H

#include <uapi/types.h>
#include <asm/context.h>

void   kprintf(const char *fmt, ...);
void   kpanic(const char *msg);
void   panic_report(const char *msg, arch_context_t *ctx);
void   kprintf_set_fb(void);
void   kprintf_flush(void);
size_t kprintf_read_log(char *dst, size_t max);
size_t kprintf_log_len(void);

#endif
