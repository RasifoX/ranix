/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* /dev/kvm: virtual machines for userspace, with the ioctl interface of
 * Linux's KVM so that QEMU and other monitors run unchanged.
 *
 * Three kinds of file, as on Linux. /dev/kvm answers questions and makes VMs;
 * a VM fd holds memory slots and makes vcpus; a vcpu fd runs the guest
 * (KVM_RUN) and maps struct kvm_run at offset 0, with port I/O data in the
 * page after it. A VM keeps the creator's address space: its slots name
 * addresses there, and only threads of that space may use it (EIO otherwise,
 * as on Linux).
 *
 * /dev/kvm is 0660 root:kvm (gid 34, Alpine's kvm group): running a guest is a
 * privilege the machine's owner hands out.
 *
 * Provenance: ioctl numbers, structures and their semantics from Linux's
 * public KVM API description (Documentation/virt/kvm/api.rst), read as an
 * interface definition. Original code; no Linux source was read. */

#include <asm/kvm.h>
#include <uapi/kvm.h>
#include <kernel/fs/devfs.h>
#include <kernel/vfs.h>
#include <kernel/proc.h>
#include <kernel/syscall.h>
#include <kernel/hal.h>
#include <kernel/kprintf.h>
#include <kernel/errno.h>
#include <kernel/mm/heap.h>
#include <kernel/mm/buddy.h>
#include <kernel/mm/pmm.h>
#include <kernel/lib/string.h>

#define KVM_DEV_RDEV ((10 << 8) | 232)   /* misc major 10, minor 232, as on Linux */
#define KVM_MMAP_PAGES 2                  /* kvm_run, then the port I/O data */

static vfs_iops_t kvm_iops = { 0 };

/* A new fd for an anonymous file with these fops, close-on-exec as Linux's
 * VM and vcpu fds are. */
static int kvm_new_fd(vfs_fops_t *fops, void *priv)
{
	vfs_inode_t *inode = kmalloc(sizeof(*inode));
	vfs_file_t  *file  = kmalloc(sizeof(*file));
	if (!inode || !file) { kfree(inode); kfree(file); return -ENOMEM; }
	memset(inode, 0, sizeof(*inode));
	inode->mode = S_IFREG | 0600;
	inode->fops = fops;
	inode->iops = &kvm_iops;
	memset(file, 0, sizeof(*file));
	file->flags = O_RDWR;
	file->priv  = priv;
	file->inode = inode;
	process_t *p = task_current();
	int fd = fd_alloc(p->fds, file);
	if (fd < 0) { kfree(file); kfree(inode); return -EMFILE; }
	fd_set_cloexec(p->fds, fd, 1);
	return fd;
}

static int kvm_check_extension(uint64_t cap)
{
	switch (cap) {
	case KVM_CAP_USER_MEMORY:
	case KVM_CAP_IMMEDIATE_EXIT:
		return 1;
	case KVM_CAP_NR_VCPUS:
	case KVM_CAP_MAX_VCPUS:
		return KVM_MAX_VCPUS;
	case KVM_CAP_NR_MEMSLOTS:
		return KVM_MAX_SLOTS;
	default:
		return 0;
	}
}

/* ── VM lifetime ──────────────────────────────────────────── */

static void kvm_vm_put(kvm_vm_t *vm)
{
	if (__atomic_sub_fetch(&vm->refs, 1, __ATOMIC_ACQ_REL) > 0) return;
	kvm_ept_free(vm);
	vm_destroy(vm->vs);
	kfree(vm);
}

/* The caller is a thread of the VM's address space. */
static int vm_owner(kvm_vm_t *vm)
{
	return task_current()->vm == vm->vs;
}

/* ── vcpu fd ──────────────────────────────────────────────── */

static int vcpu_close(vfs_file_t *f)
{
	kvm_vcpu_t *v = f->priv;
	if (!v) return 0;
	kvm_vm_t *vm = v->vm;
	mutex_lock(&vm->lock);
	vm->vcpus[v->id] = NULL;
	mutex_unlock(&vm->lock);
	vmx_vcpu_free(v);
	/* a mapping of the pages holds its own reference */
	pmm_free((void *)v->run_page);
	pmm_free((void *)v->pio_page);
	kfree(v);
	f->priv = NULL;
	kvm_vm_put(vm);
	return 0;
}

static uint64_t vcpu_mmap(vfs_file_t *f, off_t off)
{
	kvm_vcpu_t *v = f->priv;
	if (!v || off < 0 || off >= KVM_MMAP_PAGES * PAGE_SIZE) return 0;
	uint64_t pa = off < PAGE_SIZE ? v->run_page : v->pio_page;
	pmm_hold((void *)pa);   /* the mapping's reference: unmap drops it */
	return pa + ((uint64_t)off & (PAGE_SIZE - 1));
}

static int vcpu_ioctl(vfs_file_t *f, uint32_t cmd, uint64_t arg)
{
	kvm_vcpu_t *v = f->priv;
	void *uarg = (void *)(uintptr_t)arg;
	if (!v) return -EBADF;
	if (!vm_owner(v->vm)) return -EIO;
	int r = mutex_lock_killable(&v->lock);
	if (r) return r;
	switch (cmd) {
	case KVM_RUN:
		r = arg ? -EINVAL : vmx_vcpu_run(v);
		/* userspace stopped it (immediate_exit, a signal): it must see the
		 * EINTR and handle what it wanted, not have KVM_RUN run again */
		if (r == -EINTR) syscall_eintr_final();
		break;
	case KVM_GET_REGS: {
		struct kvm_regs regs;
		r = vmx_get_regs(v, &regs);
		if (!r && copy_to_user(uarg, &regs, sizeof(regs))) r = -EFAULT;
		break;
	}
	case KVM_SET_REGS: {
		struct kvm_regs regs;
		r = copy_from_user(&regs, uarg, sizeof(regs)) ? -EFAULT : vmx_set_regs(v, &regs);
		break;
	}
	case KVM_GET_SREGS: {
		struct kvm_sregs sregs;
		r = vmx_get_sregs(v, &sregs);
		if (!r && copy_to_user(uarg, &sregs, sizeof(sregs))) r = -EFAULT;
		break;
	}
	case KVM_SET_SREGS: {
		struct kvm_sregs sregs;
		r = copy_from_user(&sregs, uarg, sizeof(sregs)) ? -EFAULT : vmx_set_sregs(v, &sregs);
		break;
	}
	default:
		r = -ENOTTY;
	}
	mutex_unlock(&v->lock);
	return r;
}

static vfs_fops_t vcpu_fops = {
	.ioctl = vcpu_ioctl,
	.close = vcpu_close,
	.mmap  = vcpu_mmap,
	.mmap_cacheable = 1,   /* plain RAM the kernel also writes: map it write-back */
};

static int vcpu_create(kvm_vm_t *vm, uint64_t id)
{
	if (id >= KVM_MAX_VCPUS) return -EINVAL;
	kvm_vcpu_t *v = kmalloc(sizeof(*v));
	if (!v) return -ENOMEM;
	memset(v, 0, sizeof(*v));
	v->vm = vm;
	v->id = (int)id;
	v->lock = (mutex_t)MUTEX_INIT;
	v->run_page = (uint64_t)pmm_alloc();
	v->pio_page = (uint64_t)pmm_alloc();
	int r = -ENOMEM;
	if (!v->run_page || !v->pio_page) goto fail;
	memset((void *)PHYS_TO_VIRT(v->run_page), 0, PAGE_SIZE);
	memset((void *)PHYS_TO_VIRT(v->pio_page), 0, PAGE_SIZE);
	v->run = (struct kvm_run *)PHYS_TO_VIRT(v->run_page);
	r = vmx_vcpu_init(v);
	if (r) goto fail;

	r = mutex_lock_killable(&vm->lock);
	if (r) goto fail_vmx;
	if (vm->vcpus[id]) { mutex_unlock(&vm->lock); r = -EEXIST; goto fail_vmx; }
	__atomic_add_fetch(&vm->refs, 1, __ATOMIC_ACQ_REL);
	int fd = kvm_new_fd(&vcpu_fops, v);
	if (fd < 0) {
		mutex_unlock(&vm->lock);
		__atomic_sub_fetch(&vm->refs, 1, __ATOMIC_ACQ_REL);   /* the VM fd still holds one */
		r = fd;
		goto fail_vmx;
	}
	vm->vcpus[id] = v;
	mutex_unlock(&vm->lock);
	return fd;
fail_vmx:
	vmx_vcpu_free(v);
fail:
	if (v->run_page) pmm_free((void *)v->run_page);
	if (v->pio_page) pmm_free((void *)v->pio_page);
	kfree(v);
	return r;
}

/* ── VM fd ────────────────────────────────────────────────── */

/* KVM_SET_USER_MEMORY_REGION: add, replace or (size 0) delete one slot.
 * Slots may not overlap; replacing one first takes the old range away. */
static int vm_set_memory(kvm_vm_t *vm, const struct kvm_userspace_memory_region *m)
{
	const uint64_t pmask = PAGE_SIZE - 1;
	if (m->slot >= KVM_MAX_SLOTS || (m->flags & ~KVM_MEM_READONLY)) return -EINVAL;
	if ((m->guest_phys_addr | m->memory_size | m->userspace_addr) & pmask) return -EINVAL;
	if (m->guest_phys_addr + m->memory_size < m->guest_phys_addr ||
	    m->guest_phys_addr + m->memory_size > (1ULL << 48)) return -EINVAL;
	if (m->memory_size && !access_ok((void *)(uintptr_t)m->userspace_addr, m->memory_size)) return -EFAULT;
	if (m->flags & KVM_MEM_READONLY) return -EINVAL;   /* not offered (KVM_CAP_READONLY_MEM is 0) */

	int r = mutex_lock_killable(&vm->lock);
	if (r) return r;
	for (int i = 0; m->memory_size && i < KVM_MAX_SLOTS; i++) {
		kvm_slot_t *s = &vm->slots[i];
		if (i == (int)m->slot || !s->size) continue;
		if (m->guest_phys_addr < s->gpa + s->size && s->gpa < m->guest_phys_addr + m->memory_size) {
			r = -EEXIST;
			goto out;
		}
	}
	kvm_slot_t *s = &vm->slots[m->slot];
	if (s->size) {
		kvm_slot_t old = *s;
		s->size = 0;   /* gone before the unmap, so no vcpu maps it back */
		kvm_ept_unmap(vm, old.gpa, old.size);
	}
	s->gpa = m->guest_phys_addr;
	s->hva = m->userspace_addr;
	s->flags = m->flags;
	s->size = m->memory_size;
out:
	mutex_unlock(&vm->lock);
	return r;
}

static int vm_ioctl(vfs_file_t *f, uint32_t cmd, uint64_t arg)
{
	kvm_vm_t *vm = f->priv;
	if (!vm) return -EBADF;
	switch (cmd) {
	case KVM_CHECK_EXTENSION:
		return kvm_check_extension(arg);
	case KVM_CREATE_VCPU:
		if (!vm_owner(vm)) return -EIO;
		return vcpu_create(vm, arg);
	case KVM_SET_USER_MEMORY_REGION: {
		struct kvm_userspace_memory_region m;
		if (!vm_owner(vm)) return -EIO;
		if (copy_from_user(&m, (void *)(uintptr_t)arg, sizeof(m))) return -EFAULT;
		return vm_set_memory(vm, &m);
	}
	default:
		return -ENOTTY;
	}
}

static int vm_close(vfs_file_t *f)
{
	if (f->priv) kvm_vm_put(f->priv);
	f->priv = NULL;
	return 0;
}

static vfs_fops_t vm_fops = {
	.ioctl = vm_ioctl,
	.close = vm_close,
};

static int kvm_vm_create(uint64_t type)
{
	if (type != 0) return -EINVAL;
	process_t *p = task_current();
	if (!p->vm) return -EINVAL;
	kvm_vm_t *vm = kmalloc(sizeof(*vm));
	if (!vm) return -ENOMEM;
	memset(vm, 0, sizeof(*vm));
	vm->lock = (mutex_t)MUTEX_INIT;
	vm->refs = 1;
	int r = kvm_ept_init(vm);
	if (r) { kfree(vm); return r; }
	vm->vs = vm_get(p->vm);
	int fd = kvm_new_fd(&vm_fops, vm);
	if (fd < 0) kvm_vm_put(vm);
	return fd;
}

/* ── /dev/kvm ─────────────────────────────────────────────── */

static int kvm_dev_ioctl(vfs_file_t *f, uint32_t cmd, uint64_t arg)
{
	(void)f;
	switch (cmd) {
	case KVM_GET_API_VERSION:
		return arg ? -EINVAL : KVM_API_VERSION;
	case KVM_CREATE_VM:
		return kvm_vm_create(arg);
	case KVM_CHECK_EXTENSION:
		return kvm_check_extension(arg);
	case KVM_GET_VCPU_MMAP_SIZE:
		return arg ? -EINVAL : KVM_MMAP_PAGES * PAGE_SIZE;
	default:
		return -ENOTTY;
	}
}

static vfs_fops_t kvm_dev_fops = {
	.ioctl = kvm_dev_ioctl,
};

void kvm_init(void)
{
	int r = vmx_probe();
	if (r) {
		kprintf(KERN_INFO "kvm: not available (%d)\n", r);
		return;
	}
	devfs_register_device_gid("kvm", S_IFCHR | 0660, DEVFS_GID_KVM, KVM_DEV_RDEV, &kvm_dev_fops);
	kprintf(KERN_INFO "kvm: VT-x with EPT and unrestricted guest, /dev/kvm\n");
}
