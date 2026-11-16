/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* KVM ioeventfds: a guest write to a port or a device address that a VMM
 * registered signals an eventfd, and the guest goes on without an exit to
 * userspace. virtio devices are notified this way.
 *
 * The table is read between two guest entries without a lock: an entry is
 * filled before its file pointer is published, and a removed entry's file is
 * released only after kvm_vm_quiesce, when no vcpu can still be looking at it.
 * A write that matches nothing exits as before; QEMU signals a registered
 * eventfd itself then, so a lookup that misses an entry being added is fine.
 *
 * Provenance: semantics from Linux's public KVM API description (api.rst,
 * KVM_IOEVENTFD). Original code. */

#include <asm/kvm.h>
#include <uapi/kvm.h>
#include <kernel/eventfd.h>
#include <kernel/proc.h>
#include <kernel/vfs.h>
#include <kernel/errno.h>

static int same(const kvm_ioev_t *e, const struct kvm_ioeventfd *a)
{
	uint32_t kind = KVM_IOEVENTFD_FLAG_PIO | KVM_IOEVENTFD_FLAG_DATAMATCH;
	return e->addr == a->addr && e->len == a->len && (e->flags & kind) == (a->flags & kind) &&
	       (!(a->flags & KVM_IOEVENTFD_FLAG_DATAMATCH) || e->datamatch == a->datamatch);
}

int kvm_ioeventfd(kvm_vm_t *vm, const struct kvm_ioeventfd *a)
{
	uint32_t known = KVM_IOEVENTFD_FLAG_DATAMATCH | KVM_IOEVENTFD_FLAG_PIO | KVM_IOEVENTFD_FLAG_DEASSIGN;
	if (a->flags & ~known) return -EINVAL;
	if (a->len != 0 && a->len != 1 && a->len != 2 && a->len != 4 && a->len != 8) return -EINVAL;
	if (a->len == 0 && (a->flags & KVM_IOEVENTFD_FLAG_DATAMATCH)) return -EINVAL;
	if (a->addr + a->len < a->addr) return -EINVAL;
	vfs_file_t *f = fget(task_current()->fds, a->fd);
	if (!f) return -EBADF;
	if (!eventfd_is(f)) { fput(f); return -EINVAL; }
	int r = mutex_lock_killable(&vm->lock);
	if (r) { fput(f); return r; }
	kvm_ioev_t *free_e = NULL;
	r = (a->flags & KVM_IOEVENTFD_FLAG_DEASSIGN) ? -ENOENT : 0;
	for (int i = 0; i < KVM_MAX_IOEVENTFD; i++) {
		kvm_ioev_t *e = &vm->ioev[i];
		if (!e->file) { if (!free_e) free_e = e; continue; }
		if (!same(e, a)) continue;
		if (!(a->flags & KVM_IOEVENTFD_FLAG_DEASSIGN)) { r = -EEXIST; goto out; }
		if (e->file != f) continue;
		vfs_file_t *old = e->file;
		__atomic_store_n(&e->file, NULL, __ATOMIC_RELEASE);
		kvm_vm_quiesce(vm);
		fput(old);
		r = 0;
		goto out;
	}
	if (a->flags & KVM_IOEVENTFD_FLAG_DEASSIGN) goto out;
	if (!free_e) { r = -ENOSPC; goto out; }
	free_e->addr = a->addr;
	free_e->len = a->len;
	free_e->flags = a->flags;
	free_e->datamatch = a->datamatch;
	__atomic_store_n(&free_e->file, f, __ATOMIC_RELEASE);
	f = NULL;   /* the table keeps the reference */
out:
	mutex_unlock(&vm->lock);
	if (f) fput(f);
	return r;
}

/* A guest write of len bytes of data to addr: 1 when an ioeventfd took it */
int kvm_ioeventfd_hit(kvm_vm_t *vm, int pio, uint64_t addr, uint32_t len, uint64_t data)
{
	uint64_t mask = len >= 8 ? ~0ULL : (1ULL << (8 * len)) - 1;
	for (int i = 0; i < KVM_MAX_IOEVENTFD; i++) {
		kvm_ioev_t *e = &vm->ioev[i];
		vfs_file_t *f = __atomic_load_n(&e->file, __ATOMIC_ACQUIRE);
		if (!f || e->addr != addr || !(e->flags & KVM_IOEVENTFD_FLAG_PIO) != !pio) continue;
		if (e->len && e->len != len) continue;
		if ((e->flags & KVM_IOEVENTFD_FLAG_DATAMATCH) && e->datamatch != (data & mask)) continue;
		eventfd_signal_file(f, 1);
		return 1;
	}
	return 0;
}

/* the VM is going away: no vcpu runs */
void kvm_ioeventfd_free(kvm_vm_t *vm)
{
	for (int i = 0; i < KVM_MAX_IOEVENTFD; i++)
		if (vm->ioev[i].file) { fput(vm->ioev[i].file); vm->ioev[i].file = NULL; }
}
