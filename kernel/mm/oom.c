/* Out-of-memory killer. When a user-page allocation can't be satisfied (page
 * fault, brk/mmap growth, COW) the buddy is empty; rather than panic, pick the
 * largest-footprint user process and SIGKILL it so its exit frees memory. The
 * faulting op retries a bounded number of times, then fails (SIGSEGV/ENOMEM) —
 * never panics. Selection + kill live in proc.c (proctab/sched_lock in scope);
 * this is the policy and the callers' single entry point. */

#include <kernel/mm/oom.h>
#include <kernel/proc.h>
#include <kernel/kprintf.h>

int oom_kill_alloc(void)
{
	process_t *cur = task_current();
	process_t *v = oom_select_victim();
	if (!v) {
		kprintf("oom: no killable victim — allocation will fail\n");
		return OOM_NONE;
	}

	int self = (v == cur);
	kprintf("oom: kill pid=%u (%s) charged=%ldKB%s\n",
	        (unsigned)v->pid, v->name, v->mem_charged >> 10,
	        self ? " [self]" : "");
	oom_kill_victim(v);
	proc_put(v);
	return self ? OOM_KILLED_SELF : OOM_KILLED_OTHER;
}
