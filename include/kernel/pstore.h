#ifndef KERNEL_PSTORE_H
#define KERNEL_PSTORE_H

#include <uapi/types.h>

/* Persistent kernel log: mirrors every kprintf byte into a fixed physical RAM
 * region so a later boot (Ranix or Linux) can read why the last one died. Layout
 * and rationale: docs/watchdog_pstore.md. x86_64 only for now. */

#define PSTORE_STATE_RUNNING  1   /* no orderly end recorded: hang, reset, power loss */
#define PSTORE_STATE_REBOOT   2
#define PSTORE_STATE_POWEROFF 3
#define PSTORE_STATE_PANIC    4

#if defined(__x86_64__)
/* Validate the region against the RAM map. Returns 0 and fills base/size if the
 * caller must keep it out of the page allocator. */
int  pstore_region(uint64_t lowest_free, uint64_t *base, uint64_t *size);
void pstore_cancel(void);                 /* region lost to a later reservation */
void pstore_attach(void);                 /* start mirroring; report the last boot */
void pstore_putc(char c);                 /* caller serializes (kprintf_lock) */
void pstore_commit(void);                 /* push written bytes out of the CPU caches */
void pstore_beat(uint64_t ticks);         /* caller serializes */
void pstore_end(uint32_t state, const char *reason);  /* lock-free: machine going down */
#else
static inline void pstore_putc(char c) { (void)c; }
static inline void pstore_commit(void) {}
static inline void pstore_beat(uint64_t ticks) { (void)ticks; }
static inline void pstore_end(uint32_t state, const char *reason) { (void)state; (void)reason; }
#endif

#endif
