#include <kernel/nsproxy.h>
#include <kernel/proc.h>
#include <kernel/mm/heap.h>
#include <kernel/sync.h>
#include <kernel/lib/string.h>

uts_namespace_t init_uts_ns = {
	.refcount   = 1,
	.nodename   = "ranix",
	.domainname = "(none)",
};

static spinlock_t ns_lock;

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
	return ns;
}
