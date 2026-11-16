/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* CPUID for KVM guests: what this KVM can give a guest (KVM_GET_SUPPORTED_CPUID)
 * and the answers a vcpu's CPUID instruction gets (KVM_SET_CPUID2).
 *
 * The supported table is the host's CPUID with every feature taken out that
 * this KVM does not virtualize: VMX itself, the performance counters, the
 * thermal and power interfaces, x2APIC and the TSC deadline timer (the local
 * APIC is userspace's), PCID/INVPCID, RDTSCP/RDPID, PKU, AVX-512 and anything
 * whose state XSAVE would need beyond x87, SSE and AVX (the host saves only
 * those), machine checks, and the speculation-control MSRs. A feature that is
 * not listed is never in the guest's view, since userspace builds the guest's
 * table from this one; what a guest's CR4 turns on (OSXSAVE) follows the guest.
 *
 * Provenance: leaf and bit positions from the Intel SDM Vol. 2A (CPUID). The
 * table format and the "significant index" rule from Linux's public KVM API
 * description (api.rst, KVM_GET_SUPPORTED_CPUID). Original code. */

#include <asm/kvm.h>
#include <uapi/kvm.h>
#include <kernel/hal.h>
#include <kernel/errno.h>
#include <kernel/mm/heap.h>
#include <kernel/lib/string.h>

#define SUPPORTED_MAX 64

static struct kvm_cpuid_entry2 supported[SUPPORTED_MAX];
static uint32_t nsupported;

static void host_cpuid(uint32_t fn, uint32_t idx, uint32_t r[4])
{
	__asm__ volatile("cpuid" : "=a"(r[0]), "=b"(r[1]), "=c"(r[2]), "=d"(r[3]) : "a"(fn), "c"(idx));
}

#define BIT(n) (1U << (n))

/* what of the host's answer a guest may see */
static void filter(uint32_t fn, uint32_t idx, uint32_t r[4])
{
	switch (fn) {
	case 1:
		r[2] &= ~(BIT(2) | BIT(3) | BIT(4) | BIT(5) | BIT(6) | BIT(7) | BIT(8) |   /* DTES64 MONITOR DS-CPL VMX SMX EST TM2 */
		          BIT(15) | BIT(17) | BIT(18) | BIT(21) | BIT(24) | BIT(27));       /* PDCM PCID DCA x2APIC TSC-deadline OSXSAVE */
		r[2] |= BIT(31);                                                            /* hypervisor */
		r[3] &= ~(BIT(7) | BIT(14) | BIT(21) | BIT(22) | BIT(29) | BIT(31));         /* MCE MCA DS ACPI TM PBE */
		break;
	case 7:
		if (idx == 0) {
			r[0] = 0;   /* no subleaf 1 */
			r[1] &= BIT(0) | BIT(3) | BIT(5) | BIT(7) | BIT(8) | BIT(9) | BIT(13) |   /* FSGSBASE BMI1 AVX2 SMEP BMI2 ERMS FPU-CS/DS */
			        BIT(18) | BIT(19) | BIT(20) | BIT(23) | BIT(24) | BIT(29);        /* RDSEED ADX SMAP CLFLUSHOPT CLWB SHA */
		} else {
			r[1] = 0;
		}
		r[2] = 0;
		r[3] = 0;
		break;
	case 0xD:
		if (idx == 0) {
			r[0] &= 7;   /* x87, SSE, AVX */
			r[3] = 0;
		} else if (idx == 1) {
			r[0] &= BIT(0);   /* XSAVEOPT; not XSAVEC, XGETBV1, XSAVES */
			r[1] = r[2] = r[3] = 0;
		}
		break;
	case 0x80000001:
		r[2] &= BIT(0) | BIT(5) | BIT(8);                    /* LAHF, LZCNT, PREFETCHW */
		r[3] &= BIT(11) | BIT(20) | BIT(26) | BIT(29);       /* SYSCALL, NX, 1 GB pages, LM */
		break;
	case 0x80000007:
		r[0] = r[1] = r[2] = 0;
		r[3] &= BIT(8);   /* invariant TSC: the guest's TSC is the host's */
		break;
	case 0x80000008:
		r[1] = r[2] = r[3] = 0;   /* keep only the address sizes */
		break;
	}
}

static void add(uint32_t fn, uint32_t idx, int indexed)
{
	if (nsupported >= SUPPORTED_MAX) return;
	uint32_t r[4];
	host_cpuid(fn, idx, r);
	filter(fn, idx, r);
	struct kvm_cpuid_entry2 *e = &supported[nsupported++];
	memset(e, 0, sizeof(*e));
	e->function = fn;
	e->index = idx;
	e->flags = indexed ? KVM_CPUID_FLAG_SIGNIFCANT_INDEX : 0;
	e->eax = r[0]; e->ebx = r[1]; e->ecx = r[2]; e->edx = r[3];
}

void kvm_cpuid_init(void)
{
	uint32_t r[4];
	host_cpuid(0, 0, r);
	uint32_t max = r[0] < 0xD ? r[0] : 0xD;
	for (uint32_t fn = 0; fn <= max; fn++) {
		switch (fn) {
		case 4:   /* cache levels, until a null type */
			for (uint32_t i = 0; i < 8; i++) {
				host_cpuid(4, i, r);
				add(4, i, 1);
				if (!(r[0] & 0x1F)) break;
			}
			break;
		case 7:
			add(7, 0, 1);
			break;
		case 0xB: /* topology, until a level of type 0 */
			for (uint32_t i = 0; i < 4; i++) {
				host_cpuid(0xB, i, r);
				add(0xB, i, 1);
				if (!((r[2] >> 8) & 0xFF)) break;
			}
			break;
		case 0xD:
			add(0xD, 0, 1);
			add(0xD, 1, 1);
			add(0xD, 2, 1);
			break;
		case 5: case 6: case 9: case 0xA: case 0xC:
			break;   /* monitor/mwait, power, DCA, PMU: none for a guest */
		default:
			add(fn, 0, 0);
		}
	}
	host_cpuid(0x80000000, 0, r);
	uint32_t emax = r[0] < 0x80000008 ? r[0] : 0x80000008;
	for (uint32_t fn = 0x80000000; fn <= emax; fn++) add(fn, 0, 0);
	supported[0].eax = max;          /* the leaves we stopped at are the last ones */
	for (uint32_t i = 0; i < nsupported; i++)
		if (supported[i].function == 0x80000000) supported[i].eax = emax;
}

/* KVM_GET_SUPPORTED_CPUID: nent in, the table out; E2BIG if it does not fit */
int kvm_cpuid_supported(struct kvm_cpuid2 *ucpuid)
{
	struct kvm_cpuid2 h;
	if (copy_from_user(&h, ucpuid, sizeof(h))) return -EFAULT;
	if (h.nent < nsupported) return -E2BIG;
	h.nent = nsupported;
	if (copy_to_user(ucpuid, &h, sizeof(h)) ||
	    copy_to_user(ucpuid->entries, supported, nsupported * sizeof(supported[0])))
		return -EFAULT;
	return 0;
}

/* KVM_SET_CPUID2 */
int kvm_cpuid_set(kvm_vcpu_t *v, struct kvm_cpuid2 *ucpuid)
{
	struct kvm_cpuid2 h;
	if (copy_from_user(&h, ucpuid, sizeof(h))) return -EFAULT;
	if (h.nent > KVM_MAX_CPUID_ENTRIES) return -E2BIG;
	size_t bytes = h.nent * sizeof(struct kvm_cpuid_entry2);
	struct kvm_cpuid_entry2 *t = h.nent ? kmalloc(bytes) : NULL;
	if (h.nent && !t) return -ENOMEM;
	if (bytes && copy_from_user(t, ucpuid->entries, bytes)) { kfree(t); return -EFAULT; }
	kfree(v->cpuid);
	v->cpuid = t;
	v->ncpuid = h.nent;
	return 0;
}

/* KVM_GET_CPUID2 */
int kvm_cpuid_get(kvm_vcpu_t *v, struct kvm_cpuid2 *ucpuid)
{
	struct kvm_cpuid2 h;
	if (copy_from_user(&h, ucpuid, sizeof(h))) return -EFAULT;
	if (h.nent < v->ncpuid) return -E2BIG;
	h.nent = v->ncpuid;
	if (copy_to_user(ucpuid, &h, sizeof(h)) ||
	    (v->ncpuid && copy_to_user(ucpuid->entries, v->cpuid, v->ncpuid * sizeof(*v->cpuid))))
		return -EFAULT;
	return 0;
}

static const struct kvm_cpuid_entry2 *find(const struct kvm_cpuid_entry2 *t, uint32_t n,
                                           uint32_t fn, uint32_t idx)
{
	for (uint32_t i = 0; i < n; i++)
		if (t[i].function == fn && (!(t[i].flags & KVM_CPUID_FLAG_SIGNIFCANT_INDEX) || t[i].index == idx))
			return &t[i];
	return NULL;
}

/* The guest's CPUID: its table (the supported one until userspace sets one),
 * zeros for a leaf it lacks, and the bits that follow the guest's own state. */
void kvm_cpuid_guest(kvm_vcpu_t *v, uint32_t fn, uint32_t idx, uint32_t r[4])
{
	const struct kvm_cpuid_entry2 *e = v->cpuid ? find(v->cpuid, v->ncpuid, fn, idx)
	                                            : find(supported, nsupported, fn, idx);
	r[0] = e ? e->eax : 0;
	r[1] = e ? e->ebx : 0;
	r[2] = e ? e->ecx : 0;
	r[3] = e ? e->edx : 0;
	if (fn == 1) {
		r[2] &= ~BIT(27);
		if (v->cr4 & (1ULL << 18)) r[2] |= BIT(27);   /* OSXSAVE is CR4.OSXSAVE */
	} else if (fn == 0xD && idx == 0 && e) {
		r[1] = (v->xcr0 & 4) ? 832 : 576;   /* XSAVE area for the XCR0 now on */
	}
}
