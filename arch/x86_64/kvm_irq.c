/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* Interrupts from the VMM into a VM with the local APICs in the kernel (split
 * irqchip): MSIs, the GSI routing table that names one MSI per GSI, the GSI
 * lines the VMM's IOAPIC raises (KVM_IRQ_LINE), and irqfds, eventfds whose
 * signal raises a GSI without a trip through the VMM (virtio uses them).
 *
 * Only MSI routes exist: the IOAPIC and PIC are the VMM's, so an IRQCHIP route
 * has nothing to point at. The table and every delivery are under
 * vm->irq_lock, a spinlock: an irqfd fires from the signaller's context with
 * the eventfd's lock held.
 *
 * Provenance: semantics from Linux's public KVM API description (api.rst:
 * KVM_SET_GSI_ROUTING, KVM_IRQ_LINE, KVM_SIGNAL_MSI, KVM_IRQFD,
 * KVM_CAP_SPLIT_IRQCHIP). Original code. */

#include <asm/kvm.h>
#include <uapi/kvm.h>
#include <kernel/eventfd.h>
#include <kernel/proc.h>
#include <kernel/vfs.h>
#include <kernel/errno.h>
#include <kernel/mm/heap.h>
#include <kernel/lib/string.h>

#define KVM_MAX_ROUTES 1024

typedef struct kvm_irqfd_ent {
	eventfd_hook_t hook;
	kvm_vm_t *vm;
	vfs_file_t *file;
	uint32_t gsi;
	struct kvm_irqfd_ent *next;
} kvm_irqfd_ent_t;

/* the MSI a GSI names; caller holds irq_lock */
static int route_msi(kvm_vm_t *vm, uint32_t gsi, uint64_t *addr, uint32_t *data)
{
	for (uint32_t i = 0; i < vm->nroutes; i++) {
		struct kvm_irq_routing_entry *e = &vm->routes[i];
		if (e->gsi != gsi || e->type != KVM_IRQ_ROUTING_MSI) continue;
		*addr = ((uint64_t)e->u.msi.address_hi << 32) | e->u.msi.address_lo;
		*data = e->u.msi.data;
		return 0;
	}
	return -ENOENT;
}

/* raise gsi: its MSI goes to the APICs. 1 delivered, 0 no route. */
static int gsi_raise(kvm_vm_t *vm, uint32_t gsi)
{
	uint64_t addr;
	uint32_t data;
	uint64_t f = spin_lock_irq(&vm->irq_lock);
	int r = route_msi(vm, gsi, &addr, &data);
	spin_unlock_irq(&vm->irq_lock, f);
	if (r) return 0;
	kvm_msi_deliver(vm, addr, data);
	return 1;
}

int kvm_set_gsi_routing(kvm_vm_t *vm, struct kvm_irq_routing *u)
{
	struct kvm_irq_routing h;
	if (copy_from_user(&h, u, sizeof(h))) return -EFAULT;
	if (h.nr > KVM_MAX_ROUTES || h.flags) return -EINVAL;
	size_t bytes = h.nr * sizeof(struct kvm_irq_routing_entry);
	struct kvm_irq_routing_entry *t = h.nr ? kmalloc(bytes) : NULL;
	if (h.nr && !t) return -ENOMEM;
	if (bytes && copy_from_user(t, u->entries, bytes)) { kfree(t); return -EFAULT; }
	for (uint32_t i = 0; i < h.nr; i++)
		if (t[i].type != KVM_IRQ_ROUTING_MSI || t[i].gsi >= KVM_MAX_ROUTES || t[i].flags) { kfree(t); return -EINVAL; }
	uint64_t f = spin_lock_irq(&vm->irq_lock);
	struct kvm_irq_routing_entry *old = vm->routes;
	vm->routes = t;
	vm->nroutes = h.nr;
	spin_unlock_irq(&vm->irq_lock, f);
	kfree(old);
	return 0;
}

/* KVM_IRQ_LINE: an IOAPIC pin rose (level 1); the falling edge has no MSI */
int kvm_irq_line(kvm_vm_t *vm, struct kvm_irq_level *l)
{
	if (!vm->split_irqchip) return -ENXIO;
	l->status = l->level ? gsi_raise(vm, l->irq) : 1;
	return 0;
}

int kvm_signal_msi(kvm_vm_t *vm, const struct kvm_msi *m)
{
	if (!vm->split_irqchip) return -ENXIO;
	if (m->flags) return -EINVAL;
	kvm_msi_deliver(vm, ((uint64_t)m->address_hi << 32) | m->address_lo, m->data);
	return 1;
}

static int irqfd_fire(void *arg)
{
	kvm_irqfd_ent_t *e = arg;
	gsi_raise(e->vm, e->gsi);
	return 1;   /* the signal is the VM's: the eventfd reads empty */
}

int kvm_irqfd(kvm_vm_t *vm, const struct kvm_irqfd *a)
{
	if (!vm->split_irqchip) return -ENXIO;
	if (a->flags & ~KVM_IRQFD_FLAG_DEASSIGN) return -EINVAL;   /* no resample: no level-triggered irqfds */
	vfs_file_t *f = fget(task_current()->fds, (int)a->fd);
	if (!f) return -EBADF;
	int r = mutex_lock_killable(&vm->lock);
	if (r) { fput(f); return r; }
	if (a->flags & KVM_IRQFD_FLAG_DEASSIGN) {
		r = -ENOENT;
		for (kvm_irqfd_ent_t **p = &vm->irqfds; *p; p = &(*p)->next) {
			kvm_irqfd_ent_t *e = *p;
			if (e->file != f || e->gsi != a->gsi) continue;
			*p = e->next;
			eventfd_hook_del(e->file, &e->hook);
			fput(e->file);
			kfree(e);
			r = 0;
			break;
		}
		mutex_unlock(&vm->lock);
		fput(f);
		return r;
	}
	kvm_irqfd_ent_t *e = kmalloc(sizeof(*e));
	if (!e) { mutex_unlock(&vm->lock); fput(f); return -ENOMEM; }
	memset(e, 0, sizeof(*e));
	e->vm = vm;
	e->file = f;
	e->gsi = a->gsi;
	e->hook.fn = irqfd_fire;
	e->hook.arg = e;
	r = eventfd_hook_add(f, &e->hook);
	if (r) { mutex_unlock(&vm->lock); kfree(e); fput(f); return r; }
	e->next = vm->irqfds;
	vm->irqfds = e;   /* keeps the file reference */
	mutex_unlock(&vm->lock);
	return 0;
}

/* the VM is going away */
void kvm_irq_free(kvm_vm_t *vm)
{
	while (vm->irqfds) {
		kvm_irqfd_ent_t *e = vm->irqfds;
		vm->irqfds = e->next;
		eventfd_hook_del(e->file, &e->hook);
		fput(e->file);
		kfree(e);
	}
	kfree(vm->routes);
	vm->routes = NULL;
	vm->nroutes = 0;
}
