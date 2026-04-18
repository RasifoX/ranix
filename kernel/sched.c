#include <kernel/proc.h>
#include <kernel/hal.h>
#include <kernel/elf.h>
#include <kernel/kprintf.h>
#include <kernel/irq.h>
#include <kernel/mm/buddy.h>
#include <kernel/mm/mmu.h>
#include <kernel/lib/string.h>
static process_t *proctab[PROC_MAX];
static process_t  idle_proc;
static uint8_t    idle_kstack[KSTACK_SIZE] __attribute__((aligned(16)));
static int        proc_cnt  = 0;
static process_t *current   = NULL;

/* ── scheduler ───────────────────────────────────────────── */

static void idle_loop(void)
{
	while (1) arch_halt();
}

void sched_init(void)
{
	process_t *p = &idle_proc;
	memset(p, 0, sizeof(*p));
	p->pid   = 0;
	p->state = PROC_RUNNING;
	p->ppid  = 0;
	p->uid   = 0;
	p->gid   = 0;
	p->entry = idle_loop;
	p->next  = p;
	p->kstack = idle_kstack;
	memcpy(p->name, "idle", 5);

	uint8_t *sp_top = idle_kstack + KSTACK_SIZE;
	arch_context_t *ctx = (arch_context_t *)(sp_top - sizeof(arch_context_t));
	arch_context_init(ctx, idle_loop, (uint64_t)sp_top);
	p->ctx = ctx;

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
	return p->pid;
}

/* user address space layout */
/* User space lives above the kernel's 1GB identity map (0-0x40000000).
 * On x86_64: above 1GB boot page tables. On aarch64: above L1[3] device. */
#define USER_CODE_BASE   0x400000UL     /* 4 MB — Linux'un kullandığı adres */
#define USER_STACK_TOP   0x800000UL
#define USER_STACK_PAGES 2

pid_t proc_exec(const char *name, const void *elf_data, size_t elf_len)
{
	if (proc_cnt >= PROC_MAX) return -1;

	process_t *p = buddy_alloc(0);
	if (!p) return -1;
	memset(p, 0, sizeof(*p));

	p->pid   = proc_cnt;
	p->ppid  = current->pid;
	p->state = PROC_READY;
	p->uid   = current->uid;
	p->gid   = current->gid;

	int nlen = 0;
	while (name[nlen] && nlen < PROC_NAME_MAX - 1) nlen++;
	memcpy(p->name, name, nlen);
	p->name[nlen] = '\0';

	/* create address space */
	p->vm = vm_create();
	if (!p->vm) goto fail;

	/* load ELF into address space */
	uint64_t entry;
	if (elf_load(p->vm, elf_data, elf_len, &entry) != 0) {
		kprintf("[sched] elf_load failed\n");
		goto fail;
	}

	/* allocate and map user stack */
	for (int i = 0; i < USER_STACK_PAGES; i++) {
		void *spage = buddy_alloc(0);
		if (!spage) goto fail;
		memset(spage, 0, PAGE_SIZE);
		uint64_t sva = USER_STACK_TOP - (uint64_t)(USER_STACK_PAGES - i) * PAGE_SIZE;
		vm_map(p->vm, sva, (uint64_t)spage, VM_USER | VM_READ | VM_WRITE);
	}

	/* kernel stack for exception handling */
	p->kstack = buddy_alloc(1);
	if (!p->kstack) goto fail;

	uint8_t *ksp = p->kstack + KSTACK_SIZE;
	arch_context_t *ctx = (arch_context_t *)(ksp - sizeof(arch_context_t));
	arch_context_init_user(ctx, entry, USER_STACK_TOP);
	p->ctx = ctx;
	/* fd table — stdin/stdout/stderr as NULL for now */
	p->fds = fd_table_create();

	p->next = current->next;
	current->next = p;
	proctab[proc_cnt++] = p;
	return p->pid;

fail:
	buddy_free((uint64_t)p, 0);
	return -1;
}

process_t *proc_find(pid_t pid)
{
	if (pid < 0 || pid >= proc_cnt) return NULL;
	return proctab[pid];
}

void proc_wait(pid_t pid)
{
	process_t *p = proc_find(pid);
	while (p && p->state != PROC_DEAD)
		arch_yield();
}

void schedule(void)
{
	process_t *prev = current;

	if (prev->state == PROC_RUNNING)
		prev->state = PROC_READY;

	process_t *next = prev->next;
	while (next != prev) {
		if (next->state == PROC_READY)
			break;
		next = next->next;
	}

	if (next == prev || next->state != PROC_READY) {
		/* nothing ready — fall back to idle */
		if (prev->state == PROC_READY && prev != &idle_proc) {
			next = &idle_proc;
		} else if (prev == &idle_proc) {
			prev->state = PROC_RUNNING;
			return;
		} else {
			/* prev dead/sleeping and no ready task — go idle */
			next = &idle_proc;
		}
	}

	next->state = PROC_RUNNING;
	current     = next;
}

void yield(void)      { arch_yield(); }
void task_sleep(void) { current->state = PROC_SLEEPING; arch_yield(); }
void task_exit(void)
{
	current->state = PROC_DEAD;
	arch_yield();
}

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
