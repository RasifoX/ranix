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
 * Interrupt controllers: either all of them in the VMM (QEMU's
 * kernel-irqchip=off), or the local APICs here and the IOAPIC and PIC in the
 * VMM (kernel-irqchip=split, KVM_CAP_SPLIT_IRQCHIP, kvm_lapic.c). The PIC and
 * IOAPIC in the kernel too (KVM_CREATE_IRQCHIP) are not offered.
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
#include <kernel/time.h>
#include <asm/cpu.h>
#include <kernel/hal.h>
#include <kernel/kprintf.h>
#include <kernel/errno.h>
#include <kernel/mm/heap.h>
#include <kernel/mm/buddy.h>
#include <kernel/mm/pmm.h>
#include <kernel/lib/string.h>

#define KVM_DEV_RDEV ((10 << 8) | 232)
#define MCG_CTL_P (1ULL << 8)   /* IA32_MCG_CAP: IA32_MCG_CTL exists */   /* misc major 10, minor 232, as on Linux */
#define KVM_MMAP_PAGES 2                  /* kvm_run, then the port I/O data */
#define KVM_MAX_ROUTES 1024

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

/* An ioctl a VMM sent that this KVM lacks: said once each in the log (the
 * first few), which names what a VMM needs next. */
static int kvm_unknown(const char *fd, uint32_t cmd)
{
	static int told;
	if (told < 16) {
		told++;
		kprintf(KERN_INFO "kvm: %s ioctl 0x%x not supported\n", fd, cmd);
	}
	return -ENOTTY;
}

static int kvm_check_extension(uint64_t cap)
{
	switch (cap) {
	case KVM_CAP_HLT:
	case KVM_CAP_USER_MEMORY:
	case KVM_CAP_SET_TSS_ADDR:
	case KVM_CAP_EXT_CPUID:
	case KVM_CAP_MP_STATE:
	case KVM_CAP_DESTROY_MEMORY_REGION_WORKS:
	case KVM_CAP_USER_NMI:
	case KVM_CAP_JOIN_MEMORY_REGIONS_WORKS:
	case KVM_CAP_IOEVENTFD:
	case KVM_CAP_IOEVENTFD_ANY_LENGTH:
	case KVM_CAP_SET_IDENTITY_MAP_ADDR:
	case KVM_CAP_ADJUST_CLOCK:
	case KVM_CAP_INTERNAL_ERROR_DATA:
	case KVM_CAP_VCPU_EVENTS:
	case KVM_CAP_DEBUGREGS:
	case KVM_CAP_X86_ROBUST_SINGLESTEP:
	case KVM_CAP_XSAVE:
	case KVM_CAP_XCRS:
	case KVM_CAP_GET_TSC_KHZ:
	case KVM_CAP_IMMEDIATE_EXIT:
		return 1;
	case KVM_CAP_IRQCHIP:          /* as the split one: local APICs here (KVM_CAP_SPLIT_IRQCHIP) */
	case KVM_CAP_SPLIT_IRQCHIP:
	case KVM_CAP_SIGNAL_MSI:
	case KVM_CAP_IRQ_INJECT_STATUS:
	case KVM_CAP_IRQFD:
		return 1;
	case KVM_CAP_IRQ_ROUTING:
		return KVM_MAX_ROUTES;   /* GSIs a routing table may name */
	case KVM_CAP_MCE:
		return KVM_MCE_BANKS;   /* the most banks KVM_X86_SETUP_MCE takes */
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
	kvm_ioeventfd_free(vm);
	kvm_irq_free(vm);
	kvm_ept_free(vm);
	for (int i = 0; i < KVM_MAX_SLOTS; i++) kfree(vm->slots[i].dirty);
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
	uint64_t f2 = spin_lock_irq(&vm->irq_lock);   /* no delivery is walking it then */
	vm->vcpus[v->id] = NULL;
	spin_unlock_irq(&vm->irq_lock, f2);
	mutex_unlock(&vm->lock);
	kvm_lapic_free(v);
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

/* struct kvm_fpu is the FXSAVE image's fields, taken apart */
static void fpu_get(kvm_vcpu_t *v, struct kvm_fpu *u)
{
	const uint8_t *a = v->fpu;
	memset(u, 0, sizeof(*u));
	memcpy(&u->fcw, a + 0, 2);
	memcpy(&u->fsw, a + 2, 2);
	u->ftwx = a[4];
	memcpy(&u->last_opcode, a + 6, 2);
	memcpy(&u->last_ip, a + 8, 8);
	memcpy(&u->last_dp, a + 16, 8);
	memcpy(&u->mxcsr, a + 24, 4);
	memcpy(u->fpr, a + 32, sizeof(u->fpr));
	memcpy(u->xmm, a + 160, sizeof(u->xmm));
}

static int fpu_set(kvm_vcpu_t *v, const struct kvm_fpu *u)
{
	uint8_t *a = v->fpu;
	if (u->mxcsr & 0xFFFF0000U) return -EINVAL;
	memcpy(a + 0, &u->fcw, 2);
	memcpy(a + 2, &u->fsw, 2);
	a[4] = u->ftwx;
	memcpy(a + 6, &u->last_opcode, 2);
	memcpy(a + 8, &u->last_ip, 8);
	memcpy(a + 16, &u->last_dp, 8);
	memcpy(a + 24, &u->mxcsr, 4);
	memcpy(a + 32, u->fpr, sizeof(u->fpr));
	memcpy(a + 160, u->xmm, sizeof(u->xmm));
	a[512] |= 3;   /* XSTATE_BV: XRSTOR takes x87 and SSE from here now */
	return 0;
}

/* host TSC frequency in kHz, from the calibrated ns = ticks * mult >> 28 */
static uint32_t tsc_khz(void)
{
	return tsc_mult ? (uint32_t)((1000000ULL << 28) / tsc_mult) : 0;
}

#define COPY_IN(x)  (copy_from_user(&(x), uarg, sizeof(x)) ? -EFAULT : 0)
#define COPY_OUT(x) (copy_to_user(uarg, &(x), sizeof(x)) ? -EFAULT : 0)

/* the vcpu ioctls past the registers: events, MSRs, CPUID, FPU, debug */
static int vcpu_ioctl_more(kvm_vcpu_t *v, uint32_t cmd, void *uarg)
{
	int r;
	switch (cmd) {
	case KVM_INTERRUPT: {
		struct kvm_interrupt irq;
		if ((r = COPY_IN(irq))) return r;
		if (irq.irq >= KVM_NR_INTERRUPTS) return -EINVAL;
		if (v->irq_pending >= 0) return -EEXIST;   /* one at a time, as ready_for_interrupt_injection says */
		v->irq_pending = (int)irq.irq;
		return 0;
	}
	case KVM_NMI:
		v->nmi_pending = 1;
		return 0;
	case KVM_GET_MSRS: return vmx_get_msrs(v, uarg, 0);
	case KVM_SET_MSRS: return vmx_get_msrs(v, uarg, 1);
	case KVM_SET_CPUID2: return kvm_cpuid_set(v, uarg);
	case KVM_GET_CPUID2: return kvm_cpuid_get(v, uarg);
	case KVM_GET_FPU: {
		struct kvm_fpu fpu;
		fpu_get(v, &fpu);
		return COPY_OUT(fpu);
	}
	case KVM_SET_FPU: {
		struct kvm_fpu fpu;
		if ((r = COPY_IN(fpu))) return r;
		return fpu_set(v, &fpu);
	}
	case KVM_GET_XSAVE:
		return copy_to_user(uarg, v->fpu, PAGE_SIZE) ? -EFAULT : 0;
	case KVM_SET_XSAVE: {
		uint8_t *area = kmalloc(PAGE_SIZE);
		if (!area) return -ENOMEM;
		r = copy_from_user(area, uarg, PAGE_SIZE) ? -EFAULT : vmx_set_xsave(v, area);
		kfree(area);
		return r;
	}
	case KVM_GET_XCRS: {
		struct kvm_xcrs x;
		memset(&x, 0, sizeof(x));
		x.nr_xcrs = 1;
		x.xcrs[0].value = v->xcr0;
		return COPY_OUT(x);
	}
	case KVM_SET_XCRS: {
		struct kvm_xcrs x;
		if ((r = COPY_IN(x))) return r;
		if (x.nr_xcrs > 16 || x.flags) return -EINVAL;
		for (uint32_t i = 0; i < x.nr_xcrs; i++)
			if (x.xcrs[i].xcr != 0 || vmx_set_xcr0(v, x.xcrs[i].value)) return -EINVAL;
		return 0;
	}
	case KVM_GET_MP_STATE: {
		/* without the APIC here the VMM keeps halts and SIPIs: always runnable */
		struct kvm_mp_state mp = { v->mp_state };
		return COPY_OUT(mp);
	}
	case KVM_SET_MP_STATE: {
		struct kvm_mp_state mp;
		if ((r = COPY_IN(mp))) return r;
		if (mp.mp_state == KVM_MP_STATE_RUNNABLE) { v->mp_state = mp.mp_state; return 0; }
		if (!v->apic || mp.mp_state > KVM_MP_STATE_SIPI_RECEIVED) return -EINVAL;
		if (mp.mp_state == KVM_MP_STATE_SIPI_RECEIVED) {   /* the SIPI arrived: start at its vector */
			v->sipi_pending = 1;
			mp.mp_state = KVM_MP_STATE_INIT_RECEIVED;
		}
		v->mp_state = mp.mp_state;
		return 0;
	}
	case KVM_TPR_ACCESS_REPORTING:
	case KVM_SET_VAPIC_ADDR:
		/* QEMU's TPR patching for old 32-bit guests: it acts on TPR access
		 * reports, which this KVM never sends, so it never starts. Taken
		 * and left unused. */
		return v->apic ? 0 : -ENXIO;
	case KVM_GET_LAPIC: {
		if (!v->apic) return -ENXIO;
		struct kvm_lapic_state *st = kmalloc(sizeof(*st));
		if (!st) return -ENOMEM;
		kvm_lapic_get(v, st);
		r = copy_to_user(uarg, st, sizeof(*st)) ? -EFAULT : 0;
		kfree(st);
		return r;
	}
	case KVM_SET_LAPIC: {
		if (!v->apic) return -ENXIO;
		struct kvm_lapic_state *st = kmalloc(sizeof(*st));
		if (!st) return -ENOMEM;
		r = copy_from_user(st, uarg, sizeof(*st)) ? -EFAULT : 0;
		if (!r) kvm_lapic_set(v, st);
		kfree(st);
		return r;
	}
	case KVM_GET_VCPU_EVENTS: {
		struct kvm_vcpu_events e;
		if ((r = vmx_get_events(v, &e))) return r;
		return COPY_OUT(e);
	}
	case KVM_SET_VCPU_EVENTS: {
		struct kvm_vcpu_events e;
		if ((r = COPY_IN(e))) return r;
		return vmx_set_events(v, &e);
	}
	case KVM_GET_DEBUGREGS: {
		struct kvm_debugregs d;
		if ((r = vmx_get_debugregs(v, &d))) return r;
		return COPY_OUT(d);
	}
	case KVM_SET_DEBUGREGS: {
		struct kvm_debugregs d;
		if ((r = COPY_IN(d))) return r;
		return vmx_set_debugregs(v, &d);
	}
	case KVM_GET_TSC_KHZ:
		return (int)tsc_khz();
	case KVM_SET_TSC_KHZ:
		/* the guest's TSC runs at the host's: only that rate is accepted */
		return (uint32_t)(uintptr_t)uarg == tsc_khz() ? 0 : -EINVAL;
	case KVM_X86_SETUP_MCE: {
		uint64_t mcg_cap;
		if ((r = COPY_IN(mcg_cap))) return r;
		if ((mcg_cap & 0xFF) == 0 || (mcg_cap & 0xFF) > KVM_MCE_BANKS || (mcg_cap & ~(MCG_CTL_P | 0xFFULL)))
			return -EINVAL;
		v->mcg_cap = mcg_cap;
		v->mcg_ctl = ~0ULL;
		for (int i = 0; i < KVM_MCE_BANKS; i++) v->mce_bank[i * 4] = ~0ULL;   /* every error reported */
		return 0;
	}
	case KVM_ENABLE_CAP:
		return -EINVAL;
	default:
		return kvm_unknown("vcpu", cmd);
	}
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
		r = vcpu_ioctl_more(v, cmd, uarg);
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
	v->mp_state = KVM_MP_STATE_RUNNABLE;
	if (vm->split_irqchip) {
		r = kvm_lapic_create(v);
		if (r) goto fail_vmx;
		/* the others wait for their SIPI, as after power-on */
		if (id != 0) v->mp_state = KVM_MP_STATE_UNINITIALIZED;
	}

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
	uint64_t f2 = spin_lock_irq(&vm->irq_lock);
	vm->vcpus[id] = v;
	spin_unlock_irq(&vm->irq_lock, f2);
	mutex_unlock(&vm->lock);
	return fd;
fail_vmx:
	kvm_lapic_free(v);
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
	if (m->slot >= KVM_MAX_SLOTS || (m->flags & ~(KVM_MEM_READONLY | KVM_MEM_LOG_DIRTY_PAGES))) return -EINVAL;
	if ((m->guest_phys_addr | m->memory_size | m->userspace_addr) & pmask) return -EINVAL;
	if (m->guest_phys_addr + m->memory_size < m->guest_phys_addr ||
	    m->guest_phys_addr + m->memory_size > (1ULL << 48)) return -EINVAL;
	if (m->memory_size && !access_ok((void *)(uintptr_t)m->userspace_addr, m->memory_size)) return -EFAULT;
	if (m->flags & KVM_MEM_READONLY) return -EINVAL;   /* not offered (KVM_CAP_READONLY_MEM is 0) */

	uint64_t *bits = NULL;               /* a bit per page, whole words */
	if (m->memory_size && (m->flags & KVM_MEM_LOG_DIRTY_PAGES)) {
		uint64_t words = ((m->memory_size >> 12) + 63) / 64;
		bits = kmalloc(words * 8);
		if (!bits) return -ENOMEM;
		memset(bits, 0, words * 8);
	}
	int r = mutex_lock_killable(&vm->lock);
	if (r) { kfree(bits); return r; }
	for (int i = 0; m->memory_size && i < KVM_MAX_SLOTS; i++) {
		kvm_slot_t *s = &vm->slots[i];
		if (i == (int)m->slot || !s->size) continue;
		if (m->guest_phys_addr < s->gpa + s->size && s->gpa < m->guest_phys_addr + m->memory_size) {
			r = -EEXIST;
			goto out;
		}
	}
	kvm_slot_t *s = &vm->slots[m->slot];
	__atomic_add_fetch(&vm->slots_seq, 1, __ATOMIC_RELEASE);   /* odd: changing */
	if (s->size) {
		kvm_slot_t old = *s;
		s->size = 0;   /* gone before the unmap, so no vcpu maps it back */
		kvm_ept_unmap(vm, old.gpa, old.size);
	}
	kfree(s->dirty);
	s->dirty = bits;
	bits = NULL;
	s->gpa = m->guest_phys_addr;
	s->hva = m->userspace_addr;
	s->flags = m->flags;
	s->size = m->memory_size;
	__atomic_add_fetch(&vm->slots_seq, 1, __ATOMIC_RELEASE);
out:
	mutex_unlock(&vm->lock);
	kfree(bits);
	return r;
}

/* KVM_GET_DIRTY_LOG: the slot's pages written since the last call; they are
 * write-protected again before the bits are cleared, so a write in between is
 * either in this round or the next, never lost. */
static int vm_get_dirty_log(kvm_vm_t *vm, const struct kvm_dirty_log *d)
{
	if (d->slot >= KVM_MAX_SLOTS) return -EINVAL;
	int r = mutex_lock_killable(&vm->lock);
	if (r) return r;
	kvm_slot_t *s = &vm->slots[d->slot];
	if (!s->size || !s->dirty) { mutex_unlock(&vm->lock); return -ENOENT; }
	uint64_t words = ((s->size >> 12) + 63) / 64;
	uint64_t *copy = kmalloc(words * 8);
	if (!copy) { mutex_unlock(&vm->lock); return -ENOMEM; }
	kvm_ept_protect(vm, s->gpa, s->size);
	memcpy(copy, s->dirty, words * 8);
	memset(s->dirty, 0, words * 8);
	mutex_unlock(&vm->lock);
	r = copy_to_user((void *)(uintptr_t)d->dirty_bitmap, copy, words * 8) ? -EFAULT : 0;
	kfree(copy);
	return r;
}

/* KVM_ENABLE_CAP on the VM: only the split irqchip, before any vcpu (their
 * APICs are made with them); args[0] is the IOAPIC's pin count, its routes */
static int vm_enable_cap(kvm_vm_t *vm, const struct kvm_enable_cap *ec)
{
	if (ec->cap != KVM_CAP_SPLIT_IRQCHIP || ec->flags) return -EINVAL;
	if (ec->args[0] > KVM_MAX_ROUTES) return -EINVAL;
	int r = mutex_lock_killable(&vm->lock);
	if (r) return r;
	for (int i = 0; i < KVM_MAX_VCPUS; i++) if (vm->vcpus[i]) r = -EINVAL;
	if (!r) {
		if (vm->split_irqchip) r = -EEXIST;
		else vm->split_irqchip = 1;
	}
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
	case KVM_GET_DIRTY_LOG: {
		struct kvm_dirty_log d;
		if (!vm_owner(vm)) return -EIO;
		if (copy_from_user(&d, (void *)(uintptr_t)arg, sizeof(d))) return -EFAULT;
		return vm_get_dirty_log(vm, &d);
	}
	case KVM_SET_TSS_ADDR:
	case KVM_SET_IDENTITY_MAP_ADDR:
		/* only a VT-x without unrestricted guest runs real mode through
		 * these; this KVM requires it, so the addresses are not used */
		return 0;
	case KVM_GET_CLOCK: {
		struct kvm_clock_data c;
		memset(&c, 0, sizeof(c));
		c.clock = mono_ns() + vm->clock_off;
		return copy_to_user((void *)(uintptr_t)arg, &c, sizeof(c)) ? -EFAULT : 0;
	}
	case KVM_SET_CLOCK: {
		struct kvm_clock_data c;
		if (copy_from_user(&c, (void *)(uintptr_t)arg, sizeof(c))) return -EFAULT;
		vm->clock_off = c.clock - mono_ns();
		return 0;
	}
	case KVM_IOEVENTFD: {
		struct kvm_ioeventfd a;
		if (copy_from_user(&a, (void *)(uintptr_t)arg, sizeof(a))) return -EFAULT;
		return kvm_ioeventfd(vm, &a);
	}
	case KVM_ENABLE_CAP: {
		struct kvm_enable_cap ec;
		if (copy_from_user(&ec, (void *)(uintptr_t)arg, sizeof(ec))) return -EFAULT;
		return vm_enable_cap(vm, &ec);
	}
	case KVM_IRQ_LINE:
	case KVM_IRQ_LINE_STATUS: {
		struct kvm_irq_level l;
		if (copy_from_user(&l, (void *)(uintptr_t)arg, sizeof(l))) return -EFAULT;
		int r = kvm_irq_line(vm, &l);
		if (!r && cmd == KVM_IRQ_LINE_STATUS && copy_to_user((void *)(uintptr_t)arg, &l, sizeof(l))) r = -EFAULT;
		return r;
	}
	case KVM_SET_GSI_ROUTING:
		return vm->split_irqchip ? kvm_set_gsi_routing(vm, (struct kvm_irq_routing *)(uintptr_t)arg) : -ENXIO;
	case KVM_SIGNAL_MSI: {
		struct kvm_msi m;
		if (copy_from_user(&m, (void *)(uintptr_t)arg, sizeof(m))) return -EFAULT;
		return kvm_signal_msi(vm, &m);
	}
	case KVM_IRQFD: {
		struct kvm_irqfd a;
		if (copy_from_user(&a, (void *)(uintptr_t)arg, sizeof(a))) return -EFAULT;
		return kvm_irqfd(vm, &a);
	}
	default:
		return kvm_unknown("vm", cmd);
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
	vm->irq_lock = (spinlock_t)SPINLOCK_INIT;
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
	case KVM_GET_SUPPORTED_CPUID:
		return kvm_cpuid_supported((struct kvm_cpuid2 *)(uintptr_t)arg);
	case KVM_GET_MSR_INDEX_LIST:
		return kvm_msr_index_list((struct kvm_msr_list *)(uintptr_t)arg);
	case KVM_X86_GET_MCE_CAP_SUPPORTED: {
		uint64_t mcg_cap = MCG_CTL_P | KVM_MCE_BANKS;   /* IA32_MCG_CTL, and banks; nothing else */
		return copy_to_user((void *)(uintptr_t)arg, &mcg_cap, sizeof(mcg_cap)) ? -EFAULT : 0;
	}
	default:
		return kvm_unknown("/dev/kvm", cmd);
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
	kvm_cpuid_init();
	devfs_register_device_gid("kvm", S_IFCHR | 0660, DEVFS_GID_KVM, KVM_DEV_RDEV, &kvm_dev_fops);
	kprintf(KERN_INFO "kvm: VT-x with EPT and unrestricted guest, /dev/kvm\n");
}
