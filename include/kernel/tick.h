/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
#ifndef TICK_H
#define TICK_H

typedef void (*tick_fn_t)(void);

void tick_register(tick_fn_t fn);
void tick_notify(void);
void tick_notify_local(void); /* sched_tick only — for secondary CPUs */

/* Fine wakeups (kernel/sched.c): a sleeper's end in CLOCK_MONOTONIC ns, fired by
 * its CPU's timer set for the earliest one instead of the next tick. Set by the
 * arch when its timer runs one-shot against a nanosecond clock; 0 = tick only. */
extern int hrtimer_on;
uint64_t hrtimer_next(void);           /* this CPU's earliest fine end, or ~0 */
uint64_t hrtimer_program_at(uint64_t now_ns);   /* when to set the timer for it: a little ahead, see sched.c */
void     hrtimer_run(uint64_t now_ns); /* this CPU's timer interrupt: wake the due */
void     arch_timer_reprogram(void);   /* this CPU: timer at min(next tick, hrtimer_next()) */
void     sched_idle_wait(volatile int *flag, int deep);   /* idle: spin for a near fine wakeup, else halt */

#endif
