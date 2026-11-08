#ifndef OOM_H
#define OOM_H

struct process;

#define OOM_NONE          0   /* nothing killable; caller must fail the alloc */
#define OOM_KILLED_OTHER  1   /* killed another task; a retry may now succeed */
#define OOM_KILLED_SELF   2   /* victim is the caller; it will die on return */

/* Reclaim by killing the biggest user process. Call after a user-page
 * buddy_alloc/pmm_alloc returns NULL; then retry the alloc a bounded number of
 * times before failing the op. */
int oom_kill_alloc_ex(int can_sleep);
#define oom_kill_alloc() oom_kill_alloc_ex(0)

/* selection + kill, implemented in kernel/proc.c (proctab/sched_lock in scope) */
struct process *oom_select_victim(long *charged);
void            oom_kill_victim(struct process *v);

#endif
