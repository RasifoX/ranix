#ifndef TICK_H
#define TICK_H

typedef void (*tick_fn_t)(void);

void tick_register(tick_fn_t fn);
void tick_notify(void);
void tick_notify_local(void); /* sched_tick only — for secondary CPUs */

#endif
