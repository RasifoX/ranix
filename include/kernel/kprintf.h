#ifndef KPRINTF_H
#define KPRINTF_H

#include <uapi/types.h>
#include <asm/context.h>

/* Level tags: the first thing in a kprintf format, kprintf(KERN_INFO "mm: ...").
 * Levels are small numbers, lower is graver; ranix.loglevel=N puts messages of
 * level N and graver on the console, every message goes to the ring. */
#define KLOG_TAG     '\x7f'
#define KLOG_ERR     1
#define KLOG_WARN    2
#define KLOG_INFO    3
#define KLOG_DEBUG   4

#define KERN_ERR     "\x7f" "1"
#define KERN_WARN    "\x7f" "2"
#define KERN_INFO    "\x7f" "3"
#define KERN_DEBUG   "\x7f" "4"
#define KERN_CONT    "\x7f" "c"   /* the rest of a line a previous call began */

void   kprintf(const char *fmt, ...);
void   klog_setup(void);           /* once the command line is known */
void   kpanic(const char *msg);
void   panic_report(const char *msg, arch_context_t *ctx);
void   kprintf_set_fb(void);
void   kprintf_flush(void);
size_t kprintf_read_log(char *dst, size_t max);
size_t kprintf_log_len(void);

/* /proc/sys/kernel/dmesg_restrict (procfs.c): when set, only a privileged task
 * may read the kernel log ring; the boot log carries addresses and device
 * details useful to an attacker. Writes (logging) stay open. */
extern int dmesg_restrict;
void klog_pstore_beat(uint64_t ticks);

#endif
