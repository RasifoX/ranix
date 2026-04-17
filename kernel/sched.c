#include <kernel/proc.h>
#include <kernel/hal.h>
#include <kernel/kprintf.h>
#include <kernel/irq.h>
#include <kernel/mm/buddy.h>
#include <kernel/lib/string.h>

static process_t *proctab[PROC_MAX];
static process_t  idle_proc;
static int        proc_cnt  = 0;
static process_t *current   = NULL;

/* ── scheduler ───────────────────────────────────────────── */

void sched_init(void)
{
	process_t *p = &idle_proc;
	memset(p, 0, sizeof(*p));
	p->pid   = 0;
	p->state = PROC_RUNNING;
	p->ppid  = 0;
	p->uid   = 0;
	p->gid   = 0;
	p->next  = p;
	memcpy(p->name, "idle", 5);

	proctab[0] = p;
	current    = p;
	proc_cnt   = 1;
}

pid_t proc_create(const char *name, void (*fn)(void))
{
	if (proc_cnt >= PROC_MAX)
		return -1;

	process_t *p = buddy_alloc(0);
	if (!p) return -1;
	memset(p, 0, sizeof(*p));

	p->pid   = proc_cnt;
	p->ppid  = current->pid;
	p->state = PROC_READY;
	p->uid   = current->uid;
	p->gid   = current->gid;
	p->entry = fn;

	int nlen = 0;
	while (name[nlen] && nlen < PROC_NAME_MAX - 1) nlen++;
	memcpy(p->name, name, nlen);
	p->name[nlen] = '\0';

	/* kernel stack */
	p->kstack = buddy_alloc(1);   /* 2 pages = 8 KB */
	if (!p->kstack) { buddy_free((uint64_t)p, 0); return -1; }

	uint8_t *sp_top = p->kstack + KSTACK_SIZE;
	arch_context_t *ctx = (arch_context_t *)(sp_top - sizeof(arch_context_t));
	arch_context_init(ctx, fn, (uint64_t)sp_top);
	p->ctx = ctx;

	/* insert into scheduler ring */
	p->next = current->next;
	current->next = p;

	proctab[proc_cnt++] = p;
	kprintf("[sched] proc %d '%s'\n", (int)p->pid, p->name);
	return p->pid;
}

process_t *proc_find(pid_t pid)
{
	if (pid < 0 || pid >= proc_cnt) return NULL;
	return proctab[pid];
}

void schedule(void)
{
	process_t *prev = current;
	process_t *next = prev->next;

	while (next != prev) {
		if (next->state == PROC_READY || next->state == PROC_RUNNING)
			break;
		next = next->next;
	}

	if (next == prev || next->state != PROC_READY)
		return;

	if (prev->state == PROC_RUNNING)
		prev->state = PROC_READY;

	next->state = PROC_RUNNING;
	current     = next;
}

void yield(void)      { arch_yield(); }
void task_sleep(void) { current->state = PROC_SLEEPING; arch_yield(); }
void task_exit(void)  { current->state = PROC_DEAD;     arch_yield(); }

void task_wake(process_t *p)
{
	if (p && p->state == PROC_SLEEPING)
		p->state = PROC_READY;
}

arch_context_t *sched_get_current_ctx(void) { return current->ctx; }

process_t *task_current(void) { return current; }
int        task_count(void)   { return proc_cnt; }

process_t *task_get(int i)
{
	if (i < 0 || i >= proc_cnt) return NULL;
	return proctab[i];
}
