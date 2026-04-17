#include <kernel/sched.h>
#include <kernel/hal.h>
#include <kernel/kprintf.h>
#include <kernel/irq.h>
#include <kernel/mm/pmm.h>
#include <kernel/lib/string.h>

static task_t  tasks[TASK_MAX];
static int     task_cnt = 0;
static task_t *current_task = NULL;

static void set_name(task_t *t, const char *s)
{
	int i;
	for (i = 0; i < TASK_NAME_MAX - 1 && s[i]; i++)
		t->name[i] = s[i];
	t->name[i] = '\0';
}

void sched_init(void)
{
	task_t *t = &tasks[0];
	t->id         = 0;
	t->state      = TASK_RUNNING;
	t->ctx        = 0;
	t->entry      = 0;
	t->stack_base = 0;
	set_name(t, "idle");
	t->next = t;

	current_task = t;
	task_cnt = 1;
}

int task_create(const char *name, void (*fn)(void))
{
	if (task_cnt >= TASK_MAX)
		return -1;

	task_t *t = &tasks[task_cnt];
	t->id    = task_cnt;
	t->state = TASK_READY;
	t->entry = fn;
	set_name(t, name);

	uint8_t *p0 = (uint8_t *)pmm_alloc();
	uint8_t *p1 = (uint8_t *)pmm_alloc();
	(void)p1;
	if (!p0 || !p1)
		return -1;
	t->stack_base = p0;

	uint8_t *sp_top = p0 + TASK_STACK_SIZE;
	arch_context_t *ctx = (arch_context_t *)(sp_top - sizeof(arch_context_t));
	arch_context_init(ctx, fn, (uint64_t)sp_top);
	t->ctx = ctx;

	t->next = current_task->next;
	current_task->next = t;

	task_cnt++;
	kprintf("[SCHED] task %d '%s' created\n", (int)t->id, t->name);
	return (int)t->id;
}

void schedule(void)
{
	task_t *prev = current_task;
	task_t *next = prev->next;

	while (next != prev) {
		if (next->state == TASK_READY)
			break;
		next = next->next;
	}

	if (next == prev || next->state != TASK_READY)
		return;

	if (prev->state == TASK_RUNNING)
		prev->state = TASK_READY;

	next->state  = TASK_RUNNING;
	current_task = next;
}

void yield(void)      { arch_yield(); }
void task_sleep(void) { current_task->state = TASK_SLEEPING; arch_yield(); }

void task_wake(task_t *t)
{
	if (t->state == TASK_SLEEPING)
		t->state = TASK_READY;
}

arch_context_t *sched_get_current_ctx(void) { return current_task->ctx; }

task_t *task_current(void) { return current_task; }
int     task_count(void)   { return task_cnt; }

task_t *task_get(int index)
{
	if (index < 0 || index >= task_cnt)
		return 0;
	return &tasks[index];
}
