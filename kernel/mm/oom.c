/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* Out-of-memory killer. When a user-page allocation can't be satisfied (page
 * fault, brk/mmap growth, COW) the buddy is empty; rather than panic, pick the
 * largest-footprint user process and SIGKILL it so its exit frees memory. The
 * faulting op retries a bounded number of times, then fails (SIGSEGV/ENOMEM) —
 * never panics. Selection + kill live in proc.c (proctab/sched_lock in scope);
 * this is the policy and the callers' single entry point. */

#include <kernel/mm/oom.h>
#include <kernel/proc.h>
#include <kernel/kprintf.h>
#include <kernel/vfs.h>
#include <kernel/config.h>
#include <kernel/board.h>
#include <kernel/hal.h>
#include <kernel/time.h>

int oom_kill_alloc_ex(int can_sleep)
{
#if IS_ENABLED(CONFIG_FS_EXT2)
	if (ext2_shrink(1024)) return OOM_KILLED_OTHER;   /* clean cached blocks go first */
#endif
	process_t *cur = task_current();
	long charged = 0;
	{
		int pend = oom_victim_pending();
		if (pend == 2) return OOM_KILLED_SELF;   /* this one dies on its way out */
		if (pend == 1) {                         /* wait for the last kill to land */
			if (can_sleep) {
				task_sleep_until(kernel_ticks + TIMER_HZ / 50);
			} else {                             /* under a spinlock: spin, briefly */
				/* bounded by count too: with interrupts off on the tick CPU
				 * kernel_ticks would not move */
				uint64_t t = kernel_ticks;
				for (int k = 0; k < 2000000 && kernel_ticks - t < TIMER_HZ / 200; k++)
					cpu_relax();
			}
			return OOM_KILLED_OTHER;
		}
	}
	process_t *v = oom_select_victim(&charged);
	if (!v) {
		kprintf(KERN_ERR "oom: no killable victim, allocation will fail\n");
		return OOM_NONE;
	}

	int self = (v == cur);
	kprintf(KERN_WARN "oom: kill pid=%u (%s) charged=%ldKB%s\n",
	        (unsigned)v->pid, v->name, charged >> 10,
	        self ? " [self]" : "");
	oom_kill_victim(v);
	proc_put(v);
	return self ? OOM_KILLED_SELF : OOM_KILLED_OTHER;
}
