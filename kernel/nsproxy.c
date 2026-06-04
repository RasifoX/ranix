#include <kernel/nsproxy.h>
#include <kernel/proc.h>
#include <kernel/mm/heap.h>
#include <kernel/sync.h>
#include <kernel/lib/string.h>

uts_namespace_t init_uts_ns = {
	.refcount   = 1,
	.inum       = 0xF0000000UL,
	.nodename   = "ranix",
	.domainname = "(none)",
};

static spinlock_t ns_lock = SPINLOCK_INIT;
static unsigned long ns_inum_next = 0xF0000100UL;

unsigned long ns_inum_alloc(void)
{
	uint64_t f = spin_lock_irq(&ns_lock);
	unsigned long v = ns_inum_next++;
	spin_unlock_irq(&ns_lock, f);
	return v;
}

uts_namespace_t *current_uts(void)
{
	process_t *p = task_current();
	return (p && p->uts_ns) ? p->uts_ns : &init_uts_ns;
}

uts_namespace_t *uts_ns_get(uts_namespace_t *ns)
{
	if (!ns) ns = &init_uts_ns;
	if (ns != &init_uts_ns) {
		uint64_t f = spin_lock_irq(&ns_lock);
		ns->refcount++;
		spin_unlock_irq(&ns_lock, f);
	}
	return ns;
}

void uts_ns_put(uts_namespace_t *ns)
{
	if (!ns || ns == &init_uts_ns) return;
	uint64_t f = spin_lock_irq(&ns_lock);
	int last = (--ns->refcount == 0);
	spin_unlock_irq(&ns_lock, f);
	if (last) kfree(ns);
}

uts_namespace_t *uts_ns_clone(uts_namespace_t *old)
{
	if (!old) old = &init_uts_ns;
	uts_namespace_t *ns = kmalloc(sizeof(*ns));
	if (!ns) return NULL;
	memcpy(ns, old, sizeof(*ns));
	ns->refcount = 1;
	ns->inum = ns_inum_alloc();
	return ns;
}

/* ── time namespace ───────────────────────────────────────── */

time_namespace_t init_time_ns = { .refcount = 1, .inum = 0xF0000003UL };

time_namespace_t *current_time_ns(void)
{
	process_t *p = task_current();
	return (p && p->time_ns) ? p->time_ns : &init_time_ns;
}

time_namespace_t *time_ns_get(time_namespace_t *ns)
{
	if (!ns) ns = &init_time_ns;
	if (ns != &init_time_ns) {
		uint64_t f = spin_lock_irq(&ns_lock);
		ns->refcount++;
		spin_unlock_irq(&ns_lock, f);
	}
	return ns;
}

void time_ns_put(time_namespace_t *ns)
{
	if (!ns || ns == &init_time_ns) return;
	uint64_t f = spin_lock_irq(&ns_lock);
	int last = (--ns->refcount == 0);
	spin_unlock_irq(&ns_lock, f);
	if (last) kfree(ns);
}

time_namespace_t *time_ns_clone(time_namespace_t *old)
{
	if (!old) old = &init_time_ns;
	time_namespace_t *ns = kmalloc(sizeof(*ns));
	if (!ns) return NULL;
	memcpy(ns, old, sizeof(*ns));
	ns->refcount = 1;
	ns->inum = ns_inum_alloc();
	return ns;
}
