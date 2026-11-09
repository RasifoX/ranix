/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
#ifndef KERNEL_KWATCH_H
#define KERNEL_KWATCH_H

#include <uapi/types.h>
#include <kernel/kout.h>

/* kwatch: the kill-always-wins and kernel-stall watch (kernel/kwatch.c).
 * A killed task must be gone within seconds, and a task must not sit in one
 * syscall without yielding while others wait; a breach is named in the log,
 * in a small event ring and in the counters below. */

struct process;
struct cpu_context;
extern volatile uint64_t kwatch_unkillable_count, kwatch_kstall_count;

/* From sched_tick_local on every CPU, for the task the tick interrupted. */
void kwatch_tick_local(struct process *cur, struct cpu_context *cc);

/* From hung_task_check (~every 5 s): report tasks that outlived a SIGKILL. */
void kwatch_kill_sweep(uint64_t now);

/* Counters, events and stuck tasks as text (rescue channel, status). */
void kwatch_report(kout_t *o, int with_bt);

#endif
