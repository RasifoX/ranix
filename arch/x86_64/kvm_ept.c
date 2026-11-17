/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* Guest memory for KVM: the EPT tables that turn a guest-physical address into
 * a page of the VM owner's memory.
 *
 * A guest page is mapped on its first touch (an EPT violation): the memory slot
 * names the owner's virtual address, vm_pin_user_page makes the page present and
 * private there and holds it, and the 4 KB leaf points at it. The hold keeps the
 * page from being swapped or moved while the guest reaches it by physical
 * address; it is dropped when the slot goes away or the VM is freed. Every leaf
 * that holds a page carries EPT_PINNED, a bit the processor ignores.
 *
 * Dirty logging (KVM_MEM_LOG_DIRTY_PAGES, QEMU's VGA and migration): a logged
 * slot's pages are mapped without write access; the first write faults, sets
 * the page's bit and makes the leaf writable. KVM_GET_DIRTY_LOG hands the bits
 * out and takes write access away again (kvm_ept_protect).
 *
 * Taking a mapping away has two passes (kvm_ept_unmap): first the leaves lose
 * their access bits, then every CPU that may cache them flushes (quiesce), and
 * only then are the pages let go. A guest on another CPU therefore never
 * reaches a page after it was given back.
 *
 * Provenance: the EPT entry and EPTP formats are from the Intel SDM Vol. 3,
 * 28.3 (EPT) and 24.6.11 (EPTP); the two-pass invalidation is the usual
 * shootdown pattern. Original code. */

#include <asm/kvm.h>
#include <asm/vmx.h>
#include <uapi/kvm.h>
#include <kernel/mm/buddy.h>
#include <kernel/mm/pmm.h>
#include <kernel/mm/vm.h>
#include <kernel/lib/string.h>
#include <kernel/errno.h>
#include <kernel/hal.h>
#include <kernel/time.h>
#include <asm/cpu.h>

#define EPT_R       (1ULL << 0)
#define EPT_W       (1ULL << 1)
#define EPT_X       (1ULL << 2)
#define EPT_RWX     (EPT_R | EPT_W | EPT_X)
#define EPT_MT_WB   (6ULL << 3)   /* leaf memory type; the guest's PAT still applies */
#define EPT_PINNED  (1ULL << 11)  /* ignored by the CPU: this leaf holds a pinned page */
#define EPT_ADDR    0x000FFFFFFFFFF000ULL
#define EPTP_WB     6ULL
#define EPTP_WALK4  (3ULL << 3)   /* page-walk length minus one */
#define EPT_GPA_END (1ULL << 48)  /* what four levels translate */

static uint64_t *table(uint64_t e) { return (uint64_t *)PHYS_TO_VIRT(e & EPT_ADDR); }

/* The 4 KB leaf for gpa, allocating the tables on the way when alloc. NULL
 * when a table is missing (and !alloc) or memory ran out. */
static uint64_t *ept_leaf(kvm_vm_t *vm, uint64_t gpa, int alloc)
{
	uint64_t *t = (uint64_t *)PHYS_TO_VIRT(vm->ept_root);
	for (int level = 3; level > 0; level--) {
		uint64_t *e = &t[(gpa >> (12 + 9 * level)) & 511];
		if (!(*e & EPT_RWX)) {
			if (!alloc) return NULL;
			uint64_t pg = (uint64_t)buddy_alloc(0);
			if (!pg) return NULL;
			memset((void *)PHYS_TO_VIRT(pg), 0, PAGE_SIZE);
			*e = pg | EPT_RWX;   /* the table is zeroed before a walker can see it */
		}
		t = table(*e);
	}
	return &t[(gpa >> 12) & 511];
}

int kvm_ept_init(kvm_vm_t *vm)
{
	uint64_t pg = (uint64_t)buddy_alloc(0);
	if (!pg) return -ENOMEM;
	memset((void *)PHYS_TO_VIRT(pg), 0, PAGE_SIZE);
	vm->ept_root = pg;
	vm->eptp = pg | EPTP_WALK4 | EPTP_WB;
	/* A new VM's root page may have been an old VM's root: a CPU can still
	 * cache translations under that EPTP. cpu_gen never equal to ept_gen makes
	 * each CPU's first entry for this VM flush them. */
	vm->ept_gen = 1;
	memset(vm->cpu_gen, 0, sizeof(vm->cpu_gen));
	return 0;
}

kvm_slot_t *kvm_slot_find(kvm_vm_t *vm, uint64_t gpa)
{
	for (int i = 0; i < KVM_MAX_SLOTS; i++) {
		kvm_slot_t *s = &vm->slots[i];
		if (s->size && gpa >= s->gpa && gpa - s->gpa < s->size) return s;
	}
	return NULL;
}

/* Whether a slot covers gpa, from a vcpu that cannot take vm->lock (between
 * two guest entries, interrupts off): slots_seq is odd while vm_set_memory
 * changes the table and moves on after, so a read that saw it change is
 * retried. */
int kvm_slot_present(kvm_vm_t *vm, uint64_t gpa)
{
	uint32_t seq = __atomic_load_n(&vm->slots_seq, __ATOMIC_ACQUIRE);
	if (seq & 1) return -1;
	int found = kvm_slot_find(vm, gpa) != NULL;
	__atomic_thread_fence(__ATOMIC_ACQUIRE);
	return __atomic_load_n(&vm->slots_seq, __ATOMIC_RELAXED) == seq ? found : -1;
}

/* Map the page at gpa from its slot, for a write when write. The caller is the
 * VM owner's thread, in process context (pinning may fault the page in and sleep). */
int kvm_ept_fault(kvm_vm_t *vm, uint64_t gpa, int write)
{
	gpa &= ~(uint64_t)(PAGE_SIZE - 1);
	int r = mutex_lock_killable(&vm->lock);
	if (r) return r;
	kvm_slot_t *s = kvm_slot_find(vm, gpa);
	if (!s) { r = 0; goto out; }
	uint64_t *leaf = ept_leaf(vm, gpa, 1);
	if (!leaf) { r = -ENOMEM; goto out; }
	r = 1;
	int writable = !(s->flags & KVM_MEM_READONLY);
	/* a logged page is writable only once it is marked dirty */
	int give_w = writable && (!s->dirty || write);
	if (*leaf & EPT_RWX) {           /* mapped: another vcpu was first, or a logged page's first write */
		if (give_w && !(*leaf & EPT_W)) {
			if (s->dirty) { uint64_t n = (gpa - s->gpa) >> 12; s->dirty[n / 64] |= 1ULL << (n % 64); }
			*leaf |= EPT_W;      /* more access: no flush needed */
		}
		goto out;
	}
	uint64_t pa = vm_pin_user_page(s->hva + (gpa - s->gpa), writable);
	if (!pa) { r = -EFAULT; goto out; }
	if (give_w && s->dirty) { uint64_t n = (gpa - s->gpa) >> 12; s->dirty[n / 64] |= 1ULL << (n % 64); }
	*leaf = pa | EPT_PINNED | EPT_MT_WB | EPT_R | EPT_X | (give_w ? EPT_W : 0);
out:
	mutex_unlock(&vm->lock);
	return r;
}

/* Wait until no CPU can use an EPT translation older than this call. Each
 * vcpu, with interrupts off and its VMCS loaded, publishes guest_cpu and then
 * reads ept_gen before entering; here ept_gen moves first and then guest_cpu is
 * read, so either the vcpu sees the new generation (and flushes) or it is seen
 * running and is kicked out of the guest until it has. CPUs no vcpu runs on
 * flush when one next enters there (cpu_gen). Caller holds vm->lock. */
void kvm_vm_quiesce(kvm_vm_t *vm)
{
	uint64_t gen = __atomic_add_fetch(&vm->ept_gen, 1, __ATOMIC_SEQ_CST);
	smp_mb();
	for (int i = 0; i < KVM_MAX_VCPUS; i++) {
		kvm_vcpu_t *v = vm->vcpus[i];
		if (!v) continue;
		for (;;) {
			int cpu = v->guest_cpu;
			if (cpu < 0 || v->seen_gen == gen) break;
			smp_ipi_resched(cpu);   /* any interrupt makes the guest exit */
			cpu_relax();
		}
	}
}

/* Take [gpa, gpa+size) out of the guest and give its pages back. The slot
 * that covered it is already gone, so no vcpu maps it again meanwhile. Caller
 * holds vm->lock. */
void kvm_ept_unmap(kvm_vm_t *vm, uint64_t gpa, uint64_t size)
{
	uint64_t end = gpa + size;
	int any = 0;
	for (uint64_t a = gpa; a < end; a += PAGE_SIZE) {
		uint64_t *leaf = ept_leaf(vm, a, 0);
		if (leaf && (*leaf & EPT_PINNED)) { *leaf &= ~EPT_RWX; any = 1; }
	}
	if (!any) return;
	kvm_vm_quiesce(vm);
	for (uint64_t a = gpa; a < end; a += PAGE_SIZE) {
		uint64_t *leaf = ept_leaf(vm, a, 0);
		if (!leaf || !(*leaf & EPT_PINNED)) continue;
		vm_unpin_user_page(vm->vs, *leaf & EPT_ADDR);
		*leaf = 0;
	}
}

/* Take write access away from [gpa, gpa+size): the pages stay mapped, the next
 * write faults and is logged. Caller holds vm->lock. */
void kvm_ept_protect(kvm_vm_t *vm, uint64_t gpa, uint64_t size)
{
	int any = 0;
	for (uint64_t a = gpa; a < gpa + size; a += PAGE_SIZE) {
		uint64_t *leaf = ept_leaf(vm, a, 0);
		if (leaf && (*leaf & EPT_W)) { *leaf &= ~EPT_W; any = 1; }
	}
	if (any) kvm_vm_quiesce(vm);
}

static void ept_free_table(kvm_vm_t *vm, uint64_t tphys, int level)
{
	uint64_t *t = (uint64_t *)PHYS_TO_VIRT(tphys);
	for (int i = 0; i < 512; i++) {
		uint64_t e = t[i];
		if (level > 0 && (e & EPT_RWX)) ept_free_table(vm, e & EPT_ADDR, level - 1);
		else if (level == 0 && (e & EPT_PINNED)) vm_unpin_user_page(vm->vs, e & EPT_ADDR);
	}
	buddy_free(tphys, 0);
}

/* The last reference to the VM is gone: no vcpu exists, none runs. */
void kvm_ept_free(kvm_vm_t *vm)
{
	if (vm->ept_root) ept_free_table(vm, vm->ept_root, 3);
	vm->ept_root = 0;
}

/* kvm-clock's page (struct pvclock_vcpu_time_info, Linux's pvclock ABI): the
 * guest's TSC now and the system time then, and the factor that turns TSC
 * ticks into ns: ns = ((delta << shift) * mul) >> 32, from the host's own
 * calibration (ns = ticks * tsc_mult >> 28; the guest's TSC runs at the
 * host's). Flag 1, TSC stable: every vcpu's TSC is the host's plus one offset.
 * -EAGAIN when the page is not mapped yet: v->prefault names it. */
int kvm_pvclock_update(kvm_vcpu_t *v)
{
	if (!v->pvclock_due) return 0;
	if (!tsc_mult) { v->pvclock_due = 0; return 0; }
	uint64_t gpa = v->system_time & ~1ULL;
	uint64_t mul = tsc_mult << 4;   /* >> 28 as >> 32 */
	int8_t shift = 0;
	while (mul > 0xFFFFFFFFULL) { mul >>= 1; shift++; }
	struct {
		uint32_t version, pad0;
		uint64_t tsc_timestamp, system_time;
		uint32_t tsc_to_system_mul;
		int8_t   tsc_shift;
		uint8_t  flags, pad[2];
	} __attribute__((packed)) p;
	memset(&p, 0, sizeof(p));
	p.version = 2;   /* even: complete (one writer, the vcpu's own thread) */
	uint32_t lo, hi;
	__asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
	p.tsc_timestamp = ((uint64_t)hi << 32 | lo) + vmread(TSC_OFFSET);
	p.system_time = mono_ns() + v->vm->clock_off;
	p.tsc_to_system_mul = (uint32_t)mul;
	p.tsc_shift = shift;
	p.flags = 1;
	uint64_t missing = 0;
	int r = kvm_gpa_write(v->vm, gpa, &p, sizeof(p), &missing);
	if (r == -EAGAIN) { v->prefault = missing; v->prefault_write = 1; return -EAGAIN; }
	v->pvclock_due = 0;
	return r == -EFAULT ? 0 : r;   /* no memory there: the guest gets no clock, as on Linux */
}

/* Write guest-physical memory through the EPT, without sleeping: -EAGAIN with
 * *missing for a page that is not mapped (writable) yet. */
int kvm_gpa_write(kvm_vm_t *vm, uint64_t gpa, const void *buf, size_t n, uint64_t *missing)
{
	const uint8_t *in = buf;
	while (n) {
		if (gpa >= EPT_GPA_END) return -EFAULT;
		size_t off = gpa & (PAGE_SIZE - 1), chunk = PAGE_SIZE - off;
		if (chunk > n) chunk = n;
		uint64_t *leaf = ept_leaf(vm, gpa, 0);
		if (!leaf || !(*leaf & EPT_W)) {
			*missing = gpa & ~(uint64_t)(PAGE_SIZE - 1);
			return -EAGAIN;
		}
		memcpy((uint8_t *)PHYS_TO_VIRT(*leaf & EPT_ADDR) + off, in, chunk);
		in += chunk; gpa += chunk; n -= chunk;
	}
	return 0;
}

/* Read guest-physical memory through the EPT, without sleeping (the caller may
 * be between two guest entries with interrupts off). A page the guest has not
 * touched yet is not mapped: -EAGAIN, with *missing set to it so the caller can
 * map it and retry. */
int kvm_gpa_read(kvm_vm_t *vm, uint64_t gpa, void *buf, size_t n, uint64_t *missing)
{
	uint8_t *out = buf;
	while (n) {
		if (gpa >= EPT_GPA_END) return -EFAULT;
		size_t off = gpa & (PAGE_SIZE - 1), chunk = PAGE_SIZE - off;
		if (chunk > n) chunk = n;
		uint64_t *leaf = ept_leaf(vm, gpa, 0);
		if (!leaf || !(*leaf & EPT_R)) {
			*missing = gpa & ~(uint64_t)(PAGE_SIZE - 1);
			return -EAGAIN;
		}
		memcpy(out, (uint8_t *)PHYS_TO_VIRT(*leaf & EPT_ADDR) + off, chunk);
		out += chunk; gpa += chunk; n -= chunk;
	}
	return 0;
}
