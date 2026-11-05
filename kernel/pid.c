/*
 * PID namespace core. See include/kernel/pid_namespace.h.
 *
 * Lock order: the process-creation paths hold sched_lock and then take
 * pidns_lock (to publish the nr->process maps); nothing takes pidns_lock then
 * sched_lock, so there is no cycle. Reads of a running process's pidnrs are
 * lock-free — the vector is fixed from pidns_attach (at creation, before the
 * task runs) until the PCB is freed.
 */
#include <kernel/pid_namespace.h>
#include <kernel/proc.h>
#include <kernel/mm/heap.h>
#include <kernel/sync.h>
#include <kernel/lib/string.h>

extern unsigned long ns_inum_alloc(void);

pid_namespace_t init_pid_ns = {
	.refcount = 1, .inum = 0xF0000005UL, .parent = NULL, .level = 0,
};
static spinlock_t pidns_lock = SPINLOCK_INIT;
extern spinlock_t sched_lock;

pid_namespace_t *current_pid_ns(void)
{
	process_t *p = task_current();
	return (p && p->pid_ns) ? p->pid_ns : &init_pid_ns;
}

pid_namespace_t *pid_ns_get(pid_namespace_t *ns)
{
	if (!ns) ns = &init_pid_ns;
	if (ns != &init_pid_ns) {
		uint64_t f = spin_lock_irq(&pidns_lock);
		ns->refcount++;
		spin_unlock_irq(&pidns_lock, f);
	}
	return ns;
}

void pid_ns_put(pid_namespace_t *ns)
{
	while (ns && ns != &init_pid_ns) {
		uint64_t f = spin_lock_irq(&pidns_lock);
		int last = (--ns->refcount == 0);
		spin_unlock_irq(&pidns_lock, f);
		if (!last) return;
		pid_namespace_t *parent = ns->parent;   /* drop our ref to it next */
		if (ns->map) kfree(ns->map);
		kfree(ns);
		ns = parent;                             /* tail-recurse without nesting the lock */
	}
}

pid_namespace_t *pid_ns_create_child(pid_namespace_t *parent)
{
	if (!parent) parent = &init_pid_ns;
	if (parent->level + 1 >= PIDNS_MAX_LEVEL) return NULL;
	pid_namespace_t *ns = kmalloc(sizeof(*ns));
	if (!ns) return NULL;
	memset(ns, 0, sizeof(*ns));
	ns->refcount = 1;
	ns->inum     = ns_inum_alloc();
	ns->parent   = pid_ns_get(parent);   /* the child ns holds a ref to its parent */
	ns->level    = parent->level + 1;
	return ns;
}

/* caller holds pidns_lock */
static int pidns_map_set(pid_namespace_t *ns, int nr, process_t *p)
{
	if (nr >= ns->map_cap) {
		int ncap = nr + 16;
		process_t **nm = kmalloc((size_t)ncap * sizeof(process_t *));
		if (!nm) return -1;
		memset(nm, 0, (size_t)ncap * sizeof(process_t *));
		if (ns->map) {
			memcpy(nm, ns->map, (size_t)ns->map_cap * sizeof(process_t *));
			kfree(ns->map);
		}
		ns->map = nm; ns->map_cap = ncap;
	}
	ns->map[nr] = p;
	return 0;
}

int pidns_attach(process_t *p, pid_namespace_t *ns, int global_pid)
{
	if (!ns) ns = &init_pid_ns;

	/* chain[0]=ns ... chain[n-1]=init */
	pid_namespace_t *chain[PIDNS_MAX_LEVEL];
	int n = 0;
	for (pid_namespace_t *c = ns; c && n < PIDNS_MAX_LEVEL; c = c->parent) chain[n++] = c;

	uint64_t f = spin_lock_irq(&pidns_lock);
	p->pid_levels = n;
	for (int lvl = 0; lvl < n; lvl++) {            /* lvl 0 = init ns */
		pid_namespace_t *c = chain[n - 1 - lvl];
		int nr;
		if (c == &init_pid_ns) {
			nr = global_pid;                       /* init nr == proctab index */
		} else {
			nr = ++c->last_nr;
			if (pidns_map_set(c, nr, p) != 0) {
				spin_unlock_irq(&pidns_lock, f);
				return -1;
			}
			if (nr == 1) c->reaper = p;            /* first process = ns init */
		}
		p->pidnrs[lvl].ns = c;
		p->pidnrs[lvl].nr = nr;
	}
	p->pid_ns = ns;
	if (ns != &init_pid_ns) ns->refcount++;   /* process's own-ns ref (lock held) */
	spin_unlock_irq(&pidns_lock, f);
	return 0;
}

void pidns_detach(process_t *p)
{
	uint64_t f = spin_lock_irq(&pidns_lock);
	for (int i = 0; i < p->pid_levels; i++) {
		pid_namespace_t *c = p->pidnrs[i].ns;
		int nr = p->pidnrs[i].nr;
		if (c && c != &init_pid_ns && c->map && nr > 0 && nr < c->map_cap)
			c->map[nr] = NULL;
		if (c && c->reaper == p) c->reaper = NULL;
	}
	/* the numbers stay: wait reports a zombie to its parent by them */
	spin_unlock_irq(&pidns_lock, f);
}

int task_pid_nr_ns(process_t *p, pid_namespace_t *ns)
{
	if (!p) return 0;
	for (int i = 0; i < p->pid_levels; i++)
		if (p->pidnrs[i].ns == ns) return p->pidnrs[i].nr;
	return 0;   /* p not visible in ns */
}

int task_vpid(process_t *p)
{
	if (!p) return 0;
	if (p->pid_levels == 0) return (int)p->pid;   /* not yet attached: global */
	return p->pidnrs[p->pid_levels - 1].nr;        /* deepest level = own ns */
}

process_t *pidns_find(pid_namespace_t *ns, int nr)
{
	if (!ns) ns = &init_pid_ns;
	if (ns == &init_pid_ns) return proc_find((pid_t)nr);
	uint64_t f = spin_lock_irq(&pidns_lock);
	process_t *p = (nr > 0 && nr < ns->map_cap) ? ns->map[nr] : NULL;
	spin_unlock_irq(&pidns_lock, f);
	return p;
}

/* The map entry goes in task_exit, before the PCB can be reaped, and reap frees
 * under sched_lock: finding and pinning under that lock cannot touch a dead PCB. */
process_t *proc_get_user(pid_t nr)
{
	pid_namespace_t *ns = current_pid_ns();
	if (ns == &init_pid_ns) return proc_get(nr);
	uint64_t f = spin_lock_irq(&sched_lock);
	process_t *p = pidns_find(ns, (int)nr);
	if (p) p->refcount++;
	spin_unlock_irq(&sched_lock, f);
	return p;
}

/* A pgrp or sid is stored as a global pid. In the initial namespace the number
 * stands on its own: a group outlives its exited leader. */
pid_t pid_from_user(pid_t nr)
{
	if (current_pid_ns() == &init_pid_ns) return nr;
	process_t *p = proc_get_user(nr);
	pid_t global = p ? (pid_t)p->pid : 0;
	proc_put(p);
	return global;
}

pid_t pid_to_user(pid_t global)
{
	pid_namespace_t *ns = current_pid_ns();
	if (ns == &init_pid_ns) return global;
	process_t *p = proc_get(global);
	pid_t nr = p ? (pid_t)task_pid_nr_ns(p, ns) : 0;
	proc_put(p);
	return nr;
}
