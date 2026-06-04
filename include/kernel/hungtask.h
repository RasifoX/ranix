#ifndef KERNEL_HUNGTASK_H
#define KERNEL_HUNGTASK_H

#include <uapi/types.h>

/* khungtaskd parity: a always-on, cheap safety net that fires when a task the
 * scheduler believes is making progress stops doing so. The check runs from the
 * timer tick (CPU0); it is O(nproc) but gated to one sweep every few seconds, so
 * the steady-state cost is a couple of comparisons per process per ~5s. */

struct process;

/* Per-sweep scan from the tick. Caller must NOT hold sched_lock (we take it).
 * Progress is read from p->ticks (the tick bumps it for the running task), so a
 * task that ran at all since the last sweep clears its own stall window — no
 * per-switch bookkeeping needed. */
void hung_task_check(void);

/* Times the detector has emitted its marker (for the boot self-test and any
 * future introspection). Monotonic, never cleared. */
extern volatile uint64_t hung_task_count;

/* Boot self-test (mirrors atomic_selftest): drives the detector against a
 * synthetic blocked task to prove it fires, and a healthy one to prove it does
 * not, then emits a greppable marker. Panics on a logic error. */
void hung_task_selftest(void);

#endif
