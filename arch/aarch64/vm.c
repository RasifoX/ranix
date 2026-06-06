#include <kernel/mm/vm.h>
#include <kernel/sync.h>
#include <kernel/mm/buddy.h>
#include <kernel/mm/pmm.h>
#include <kernel/mm/mmu.h>
#include <kernel/mm/hugetlb.h>
#include <kernel/lib/string.h>
#include <kernel/kprintf.h>
#include <kernel/bug.h>
#include <asm/mmu.h>

#define PT_ENTRIES  512
#define L12_TABLE   (PTE_VALID | PTE_TABLE)
#define L3_PAGE     (PTE_VALID | (1UL << 1) | PTE_AF)

/* Output-address field of a descriptor (bits 47:12). Masks off the low bits
 * and the upper attributes — PXN/UXN/NOFREE at bits 53/54/55 — so a leaf
 * entry's address survives extraction. */
#define PA_MASK     0x0000FFFFFFFFF000UL
#define PA_2M_MASK  0x0000FFFFFFE00000UL
#define PA_1G_MASK  0x0000FFFFC0000000UL

/* Execute-never bits for a leaf. User pages are never privileged-executable
 * (PXN: no ret2usr), and unprivileged-executable only when VM_EXEC. Kernel
 * mappings (ioremap/device) are never executable from either level. */
static inline uint64_t pte_xn(uint32_t flags)
{
	if (!(flags & VM_USER)) return PTE_UXN | PTE_PXN;
	return PTE_PXN | ((flags & VM_EXEC) ? 0 : PTE_UXN);
}

extern uint64_t kernel_l1[];

static uint64_t alloc_table(void)
{
	void *p = buddy_alloc(0);
	if (!p) return 0;
	memset((void *)PHYS_TO_VIRT(p), 0, PAGE_SIZE);
	return (uint64_t)p;
}

/* 48-bit VA / 4-level: vs->root is the L0 table. User VA sits under L0[0];
 * the kernel image + direct map under L0[256..510] and ioremap under L0[511].
 * The L1/L2/L3 walk below is unchanged — only the L0 slot is selected by the
 * address. */
static uint64_t user_l1(vm_space_t *vs)
{
	uint64_t *l0 = (uint64_t *)PHYS_TO_VIRT((uint64_t)vs->root);
	return l0[0] & ~0xFFFUL;
}

static uint64_t l1_for(vm_space_t *vs, uint64_t vaddr, int alloc)
{
	uint64_t *l0 = (uint64_t *)PHYS_TO_VIRT((uint64_t)vs->root);
	uint64_t l0i = (vaddr >> 39) & 0x1FF;
	if (l0[l0i] & PTE_VALID) return l0[l0i] & ~0xFFFUL;
	if (!alloc) return 0;
	uint64_t l1 = alloc_table();
	if (!l1) return 0;
	l0[l0i] = l1 | L12_TABLE;
	return l1;
}

/*
 * ASID allocator (8-bit, TCR_EL1.AS=0). Each live user address space gets a
 * unique tag 1..255 so context switches need no TLB flush. ASID 0 is the
 * "flush-on-switch" sentinel: the kernel space and the rare pool-exhausted
 * fallback use it, so two spaces can never alias under one tag.
 *
 * On free we broadcast `tlbi aside1is` for the tag, dropping its entries on
 * every CPU before it can be reused — this is what keeps reuse safe on SMP
 * without Linux's generation/reserved-asid machinery.
 */
#define ASID_MAX 256
static uint64_t asid_bitmap[ASID_MAX / 64];   /* bit i set = ASID i in use */
static spinlock_t asid_lock = SPINLOCK_INIT;

static uint32_t asid_alloc(void)
{
	uint64_t f = spin_lock_irq(&asid_lock);
	for (int i = 1; i < ASID_MAX; i++) {        /* 0 reserved as sentinel */
		if (!(asid_bitmap[i >> 6] & (1UL << (i & 63)))) {
			asid_bitmap[i >> 6] |= (1UL << (i & 63));
			spin_unlock_irq(&asid_lock, f);
			return (uint32_t)i;
		}
	}
	spin_unlock_irq(&asid_lock, f);
	return 0;   /* pool exhausted → flush-on-switch fallback */
}

static void asid_free(uint32_t asid)
{
	if (asid == 0) return;
	/* drop this tag's TLB entries on every CPU so a future reuse is clean */
	__asm__ volatile("dsb ishst\n tlbi aside1is, %0\n dsb ish\n isb"
	                 :: "r"((uint64_t)asid << 48) : "memory");
	uint64_t f = spin_lock_irq(&asid_lock);
	asid_bitmap[asid >> 6] &= ~(1UL << (asid & 63));
	spin_unlock_irq(&asid_lock, f);
}

vm_space_t *vm_create(void)
{
	void *vs_phys = buddy_alloc(0);
	if (!vs_phys) return NULL;
	vm_space_t *vs = (vm_space_t *)PHYS_TO_VIRT(vs_phys);
	vs->root = NULL;
	vs->fault_lock = (spinlock_t)SPINLOCK_INIT;

	uint64_t l0_phys = alloc_table();
	uint64_t l1_phys = alloc_table();
	if (!l0_phys || !l1_phys) {
		if (l0_phys) buddy_free(l0_phys, 0);
		if (l1_phys) buddy_free(l1_phys, 0);
		buddy_free(VIRT_TO_PHYS(vs), 0);
		return NULL;
	}
	((uint64_t *)PHYS_TO_VIRT(l0_phys))[0] = l1_phys | L12_TABLE;
	vs->root = (uint64_t *)l0_phys;

	vs->refcount = 1;
	vs->asid = asid_alloc();
	vs->magic = VM_SPACE_MAGIC;
	return vs;
}

vm_space_t *vm_get(vm_space_t *vs)
{
	/* Atomic: threads share a vm; concurrent create/exit on different CPUs
	 * would otherwise race this refcount and free the page tables early. */
	if (vs) { KASSERT(vs->magic == VM_SPACE_MAGIC); atomic_fetch_add32(&vs->refcount, 1); }
	return vs;
}

void vm_destroy(vm_space_t *vs)
{
	if (!vs) return;
	KASSERT(vs->magic == VM_SPACE_MAGIC);
	if (atomic_sub_fetch32(&vs->refcount, 1) > 0) return;

	asid_free(vs->asid);   /* invalidates this tag's TLB entries on all CPUs */

	if (!vs->root) { vs->magic = 0; buddy_free(VIRT_TO_PHYS(vs), 0); return; }

	uint64_t l1_phys = user_l1(vs);   /* under L0[0] */
	uint64_t *l1 = (uint64_t *)PHYS_TO_VIRT(l1_phys);

	for (int i = 0; i < PT_ENTRIES; i++) {
		if (!(l1[i] & PTE_VALID)) continue;
		if ((l1[i] & 3) != 3) {
			/* 1 GB L1 block */
			if (l1[i] & PTE_USER) huge1g_put(l1[i] & PA_1G_MASK);
			continue;
		}

		uint64_t l2_phys = l1[i] & ~0xFFFUL;
		uint64_t *l2 = (uint64_t *)PHYS_TO_VIRT(l2_phys);
		for (int j = 0; j < 512; j++) {
			if (!(l2[j] & PTE_VALID)) continue;
			if ((l2[j] & 3) != 3) {
				/* 2MB block */
				if (l2[j] & PTE_USER) {
					huge2m_account(-1);
					uint64_t pa = l2[j] & PA_2M_MASK;
					for (int k = 0; k < 512; k++)
						pmm_free((void *)(pa + (uint64_t)k * PAGE_SIZE));
				}
				continue;
			}

			uint64_t l3_phys = l2[j] & ~0xFFFUL;
			uint64_t *l3 = (uint64_t *)PHYS_TO_VIRT(l3_phys);
			int is_user_l3 = 0;
			for (int k = 0; k < 512; k++) {
				if (!(l3[k] & PTE_VALID)) continue;
				if (l3[k] & PTE_USER) {
					is_user_l3 = 1;
					if (!(l3[k] & PTE_NOFREE))
						pmm_free((void *)(l3[k] & PA_MASK));
				}
			}
			/* Don't free kernel-inherited L3 tables (e.g. framebuffer split).
			 * They are shared via the private L2 copy and must not be recycled. */
			if (is_user_l3) buddy_free(l3_phys, 0);
		}
		buddy_free(l2_phys, 0);
	}

	buddy_free(l1_phys, 0);            /* user L1 */
	buddy_free((uint64_t)vs->root, 0); /* L0 */
	vs->magic = 0;                     /* poison before free: trips a later use-after-free */
	buddy_free(VIRT_TO_PHYS(vs), 0);
}

int vm_map(vm_space_t *vs, uint64_t vaddr, uint64_t paddr, uint32_t flags)
{
	uint64_t l1p = l1_for(vs, vaddr, 1);
	if (!l1p) return -1;
	uint64_t *l1 = (uint64_t *)PHYS_TO_VIRT(l1p);
	uint64_t l1i = (vaddr >> 30) & 0x1FF;
	uint64_t l2i = (vaddr >> 21) & 0x1FF;
	uint64_t l3i = (vaddr >> 12) & 0x1FF;

	uint64_t l2_phys;
	if (l1[l1i] & PTE_VALID) {
		if ((l1[l1i] & 3) != 3) return -1;
		l2_phys = l1[l1i] & ~0xFFFUL;
	} else {
		l2_phys = alloc_table();
		if (!l2_phys) return -1;
		l1[l1i] = l2_phys | L12_TABLE;
	}

	uint64_t *l2 = (uint64_t *)PHYS_TO_VIRT(l2_phys);
	uint64_t l3_phys;
	if (l2[l2i] & PTE_VALID) {
		if ((l2[l2i] & 3) != 3) {
			/* 2MB L2 block — split into 512 4KB L3 pages */
			uint64_t blk  = l2[l2i];
			uint64_t base = blk & PA_2M_MASK;
			uint64_t attr = (blk & 0xFFCUL) | (blk & (PTE_PXN | PTE_UXN | PTE_NOFREE))
			              | PTE_VALID | (1UL << 1);
			l3_phys = alloc_table();
			if (!l3_phys) return -1;
			uint64_t *l3_virt = (uint64_t *)PHYS_TO_VIRT(l3_phys);
			for (int j = 0; j < PT_ENTRIES; j++)
				l3_virt[j] = (base + (uint64_t)j * PAGE_SIZE) | attr;
			l2[l2i] = l3_phys | L12_TABLE;
			uint64_t va2m = vaddr & ~0x1FFFFFUL;
			__asm__ volatile("dsb ishst; tlbi vaae1is, %0; dsb ish; isb"
			                 :: "r"(va2m >> 12) : "memory");
		} else {
			l3_phys = l2[l2i] & ~0xFFFUL;
		}
	} else {
		l3_phys = alloc_table();
		if (!l3_phys) return -1;
		l2[l2i] = l3_phys | L12_TABLE;
	}

	uint64_t *l3 = (uint64_t *)PHYS_TO_VIRT(l3_phys);

	uint64_t attr = PTE_ATTR_NORMAL;
	if (flags & VM_NOCACHE)     attr = PTE_ATTR_NORMAL_NC;
	if (flags & VM_DEVICE)      attr = PTE_ATTR_DEVICE;
	if (flags & VM_USER)        attr |= PTE_USER | PTE_NG;
	if (!(flags & VM_WRITE))    attr |= PTE_RO;
	if (flags & VM_NOFREE)      attr |= PTE_NOFREE;   /* shared page: don't free on teardown */
	attr |= pte_xn(flags);

	l3[l3i] = (paddr & ~0xFFFUL) | L3_PAGE | attr;

	__asm__ volatile(
		"dsb ishst\n"
		"tlbi vaae1is, %0\n"
		"dsb ish\n"
		"isb\n"
		:: "r"(vaddr >> 12)
	);
	return 0;
}

/* Map a 2 MB block at L2. paddr must be 2 MB-aligned. */
int vm_map_huge(vm_space_t *vs, uint64_t vaddr, uint64_t paddr, uint32_t flags)
{
	uint64_t l1p = l1_for(vs, vaddr, 1);
	if (!l1p) return -1;
	uint64_t *l1 = (uint64_t *)PHYS_TO_VIRT(l1p);
	uint64_t l1i = (vaddr >> 30) & 0x1FF;
	uint64_t l2i = (vaddr >> 21) & 0x1FF;

	uint64_t l2_phys;
	if (l1[l1i] & PTE_VALID) {
		if ((l1[l1i] & 3) != 3) return -1;
		l2_phys = l1[l1i] & ~0xFFFUL;
	} else {
		l2_phys = alloc_table();
		if (!l2_phys) return -1;
		l1[l1i] = l2_phys | L12_TABLE;
	}

	uint64_t attr = PTE_ATTR_NORMAL;
	if (flags & VM_NOCACHE)  attr = PTE_ATTR_NORMAL_NC;
	if (flags & VM_DEVICE)   attr = PTE_ATTR_DEVICE;
	if (flags & VM_USER)     attr |= PTE_USER | PTE_NG;
	if (!(flags & VM_WRITE)) attr |= PTE_RO;
	attr |= pte_xn(flags);

	/* L2 block descriptor: valid + bit[1]=0 (block, not table) + AF. */
	uint64_t *l2 = (uint64_t *)PHYS_TO_VIRT(l2_phys);
	uint64_t old = l2[l2i];
	l2[l2i] = (paddr & ~0x1FFFFFUL) | PTE_VALID | PTE_AF | attr;

	__asm__ volatile("dsb ishst; tlbi vaae1is, %0; dsb ish; isb"
	                 :: "r"((vaddr & ~0x1FFFFFUL) >> 12) : "memory");
	/* replacing a 4K leaf table (THP collapse): reclaim the orphaned table */
	if ((old & PTE_VALID) && (old & 3) == 3) buddy_free(old & ~0xFFFUL, 0);
	if (flags & VM_USER) huge2m_account(1);
	return 0;
}

/* Map a 1 GB block at L1. paddr must be 1 GB-aligned. */
int vm_map_huge_1g(vm_space_t *vs, uint64_t vaddr, uint64_t paddr, uint32_t flags)
{
	uint64_t l1p = l1_for(vs, vaddr, 1);
	if (!l1p) return -1;
	uint64_t *l1 = (uint64_t *)PHYS_TO_VIRT(l1p);
	uint64_t l1i = (vaddr >> 30) & 0x1FF;

	if ((l1[l1i] & PTE_VALID) && (l1[l1i] & 3) == 3) return -1;  /* a table is there */

	uint64_t attr = PTE_ATTR_NORMAL;
	if (flags & VM_NOCACHE)  attr = PTE_ATTR_NORMAL_NC;
	if (flags & VM_DEVICE)   attr = PTE_ATTR_DEVICE;
	if (flags & VM_USER)     attr |= PTE_USER | PTE_NG;
	if (!(flags & VM_WRITE)) attr |= PTE_RO;
	attr |= pte_xn(flags);

	l1[l1i] = (paddr & ~0x3FFFFFFFUL) | PTE_VALID | PTE_AF | attr;

	__asm__ volatile("dsb ishst; tlbi vaae1is, %0; dsb ish; isb"
	                 :: "r"((vaddr & ~0x3FFFFFFFUL) >> 12) : "memory");
	return 0;
}

void vm_unmap(vm_space_t *vs, uint64_t vaddr)
{
	uint64_t l1i = (vaddr >> 30) & 0x1FF;
	uint64_t l2i = (vaddr >> 21) & 0x1FF;
	uint64_t l3i = (vaddr >> 12) & 0x1FF;

	/* Serialize against concurrent faults in a shared address space (threads),
	 * so a sibling COW and this unmap cannot race the same leaf PTE. */
	uint64_t lf  = spin_lock_irq(&vs->fault_lock);
	uint64_t pte = 0, huge_base = 0, huge1g = 0;

	uint64_t l1p = l1_for(vs, vaddr, 0);
	if (!l1p) { spin_unlock_irq(&vs->fault_lock, lf); return; }
	uint64_t *l1 = (uint64_t *)PHYS_TO_VIRT(l1p);
	if ((l1[l1i] & PTE_VALID) && (l1[l1i] & 3) != 3) {
		/* 1 GB L1 block */
		if (l1[l1i] & PTE_USER) huge1g = l1[l1i] & PA_1G_MASK;
		l1[l1i] = 0;
		__asm__ volatile("dsb ishst; tlbi vaae1is, %0; dsb ish; isb"
		                 :: "r"((vaddr & ~0x3FFFFFFFUL) >> 12) : "memory");
		spin_unlock_irq(&vs->fault_lock, lf);
		if (huge1g) huge1g_put(huge1g);
		return;
	}
	if (l1[l1i] & PTE_VALID) {
		uint64_t *l2 = (uint64_t *)PHYS_TO_VIRT(l1[l1i] & ~0xFFFUL);
		if (l2[l2i] & PTE_VALID) {
			if ((l2[l2i] & 3) != 3) {
				/* 2 MB block */
				if (l2[l2i] & PTE_USER) huge_base = l2[l2i] & PA_2M_MASK;
				l2[l2i] = 0;
				__asm__ volatile("dsb ishst; tlbi vaae1is, %0; dsb ish; isb"
				                 :: "r"((vaddr & ~0x1FFFFFUL) >> 12) : "memory");
			} else {
				uint64_t *l3 = (uint64_t *)PHYS_TO_VIRT(l2[l2i] & ~0xFFFUL);
				pte = l3[l3i];
				l3[l3i] = 0;
				__asm__ volatile(
					"dsb ishst\n"
					"tlbi vaae1is, %0\n"
					"dsb ish\n"
					"isb\n"
					:: "r"(vaddr >> 12)
				);
			}
		}
	}

	spin_unlock_irq(&vs->fault_lock, lf);

	/* Free the physical page(s) if user-owned (outside the lock; pmm has its own). */
	if (huge_base) {
		huge2m_account(-1);
		for (int i = 0; i < 512; i++)
			pmm_free((void *)(huge_base + (uint64_t)i * PAGE_SIZE));
	} else if ((pte & PTE_VALID) && (pte & PTE_USER))
		pmm_free((void *)(pte & PA_MASK));
}

uint64_t arch_vm_get_paddr(uint64_t *l0_phys, uint64_t vaddr)
{
	/* Caller passes vs->root (the L0); descend the address's L0 slot. */
	uint64_t *l0 = (uint64_t *)PHYS_TO_VIRT((uint64_t)l0_phys);
	uint64_t l0i = (vaddr >> 39) & 0x1FF;
	if (!(l0[l0i] & PTE_VALID)) return 0;
	uint64_t *l1 = (uint64_t *)PHYS_TO_VIRT(l0[l0i] & ~0xFFFUL);
	uint64_t l1i = (vaddr >> 30) & 0x1FF;
	uint64_t l2i = (vaddr >> 21) & 0x1FF;
	uint64_t l3i = (vaddr >> 12) & 0x1FF;

	if (!(l1[l1i] & PTE_VALID)) return 0;
	if ((l1[l1i] & 3) != 3) return (l1[l1i] & PA_1G_MASK) | (vaddr & 0x3FFFFFFFUL);

	uint64_t *l2 = (uint64_t *)PHYS_TO_VIRT(l1[l1i] & ~0xFFFUL);
	if (!(l2[l2i] & PTE_VALID)) return 0;
	if ((l2[l2i] & 3) != 3) return (l2[l2i] & PA_2M_MASK) | (vaddr & 0x1FFFFFUL);

	uint64_t *l3 = (uint64_t *)PHYS_TO_VIRT(l2[l2i] & ~0xFFFUL);
	if (!(l3[l3i] & PTE_VALID)) return 0;
	return (l3[l3i] & PA_MASK);
}

/* The PTE-downgrade paths in this file already broadcast `tlbi ...is`
 * (inner-shareable), which the hardware propagates to every core, so a
 * software cross-CPU shootdown is unnecessary here. */
void arch_tlb_flush_range(vm_space_t *vs, uint64_t start, uint64_t end)
{
	(void)vs; (void)start; (void)end;
}

void arch_vm_switch(uint64_t root_phys, uint32_t asid)
{
	/* Load TTBR0 with the space's ASID in [63:48]. User PTEs are non-global
	 * (PTE_NG), so they are tagged by ASID and a switch between two tagged
	 * spaces needs NO TLB flush — the tags keep their entries apart. Only the
	 * ASID-0 sentinel (kernel / pool-exhausted fallback) flushes, since
	 * untagged spaces could otherwise alias. Reuse of a freed tag is made safe
	 * by the broadcast tlbi in asid_free(). */
	uint64_t ttbr = root_phys | ((uint64_t)asid << 48);
	if (asid != 0) {
		__asm__ volatile("msr ttbr0_el1, %0\n isb\n" :: "r"(ttbr));
	} else {
		__asm__ volatile(
			"msr ttbr0_el1, %0\n"
			"isb\n"
			"tlbi vmalle1\n"
			"dsb nsh\n"
			"isb\n"
			:: "r"(ttbr)
		);
	}
}

vm_space_t *vm_copy(vm_space_t *vs)
{
	void *new_vs_phys = buddy_alloc(0);
	if (!new_vs_phys) return NULL;
	vm_space_t *new_vs = (vm_space_t *)PHYS_TO_VIRT(new_vs_phys);
	new_vs->root = NULL;
	new_vs->refcount = 1;
	new_vs->asid = asid_alloc();
	new_vs->fault_lock = (spinlock_t)SPINLOCK_INIT;
	new_vs->magic = VM_SPACE_MAGIC;

	uint64_t l0_phys     = alloc_table();   /* new L0 */
	uint64_t l1_new_root = alloc_table();   /* new user L1 */
	if (!l0_phys || !l1_new_root) {
		if (l0_phys) buddy_free(l0_phys, 0);
		if (l1_new_root) buddy_free(l1_new_root, 0);
		goto oom;
	}
	((uint64_t *)PHYS_TO_VIRT(l0_phys))[0] = l1_new_root | L12_TABLE;
	new_vs->root = (uint64_t *)l0_phys;

	/* Lock the parent across the walk: sibling threads sharing this space may
	 * be faulting concurrently, and vm_copy both reads and write-protects the
	 * parent's leaf PTEs (for COW) while bumping page refcounts. */
	uint64_t lf = spin_lock_irq(&vs->fault_lock);

	uint64_t *l1_old = (uint64_t *)PHYS_TO_VIRT(user_l1(vs));
	uint64_t *l1_new = (uint64_t *)PHYS_TO_VIRT(l1_new_root);

	/* Walk the whole 512 GB user L1: low user mappings live in the first two
	 * 1 GB slots, 1 GB hugepages in their own high window. Empty slots skip. */
	for (int i = 0; i < PT_ENTRIES; i++) {
		if (!(l1_old[i] & PTE_VALID)) continue;
		if ((l1_old[i] & 3) != 3) {
			/* 1 GB L1 block: COW it — write-protect both + bump pool refcount;
			 * a write fault copies the whole 1 GB. */
			uint64_t entry = l1_old[i];
			if (entry & PTE_USER) {
				entry |= PTE_RO;
				l1_old[i] = entry;
				huge1g_get(entry & PA_1G_MASK);
			}
			l1_new[i] = entry;
			continue;
		}

		uint64_t *l2_old = (uint64_t *)PHYS_TO_VIRT(l1_old[i] & ~0xFFFUL);
		uint64_t l2_new_phys = alloc_table();
		if (!l2_new_phys) { spin_unlock_irq(&vs->fault_lock, lf); goto oom; }
		uint64_t *l2_new = (uint64_t *)PHYS_TO_VIRT(l2_new_phys);
		l1_new[i] = l2_new_phys | L12_TABLE;

		for (int j = 0; j < 512; j++) {
			if (!(l2_old[j] & PTE_VALID)) continue;
			if ((l2_old[j] & 3) != 3) {
				uint64_t entry = l2_old[j];
				if (entry & PTE_USER) {
					entry |= PTE_RO;
					l2_old[j] = entry;
					uint64_t pa = entry & PA_2M_MASK;
					for (int k = 0; k < 512; k++)
						pmm_hold((void *)(pa + (uint64_t)k * PAGE_SIZE));
				}
				l2_new[j] = entry;
				continue;
			}

			uint64_t *l3_old = (uint64_t *)PHYS_TO_VIRT(l2_old[j] & ~0xFFFUL);
			uint64_t l3_new_phys = alloc_table();
			if (!l3_new_phys) { spin_unlock_irq(&vs->fault_lock, lf); goto oom; }
			uint64_t *l3_new = (uint64_t *)PHYS_TO_VIRT(l3_new_phys);
			l2_new[j] = l3_new_phys | L12_TABLE;

			for (int k = 0; k < 512; k++) {
				if (!(l3_old[k] & PTE_VALID)) continue;
				uint64_t entry = l3_old[k];
				if ((entry & PTE_USER) && !(entry & PTE_NOFREE)) {
					entry |= PTE_RO;
					l3_old[k] = entry;
					pmm_hold((void *)(entry & PA_MASK));
				}
				l3_new[k] = entry;
			}
		}
	}

	spin_unlock_irq(&vs->fault_lock, lf);

	__asm__ volatile("dsb ishst; tlbi vmalle1is; dsb ish; isb" ::: "memory");

	return new_vs;

oom:
	vm_destroy(new_vs);
	return NULL;
}
