/* vDSO kernel side: copy the built vDSO image into physical pages, map it (and
 * the shared vvar timekeeping page below it) into every process at exec, and
 * refresh vvar from the timer tick. The user-space half is vdso/vdso.c; the
 * layout contract (VVAR_VADDR/VDSO_VADDR, struct vdso_vvar) is uapi/vdso.h. */
#include <kernel/mm/buddy.h>
#include <kernel/mm/pmm.h>
#include <kernel/mm/vm.h>
#include <kernel/tick.h>
#include <kernel/kprintf.h>
#include <uapi/vdso.h>
#include <asm/mmu.h>

extern void ranix_walltime(int64_t *sec, int64_t *nsec);
extern void ranix_monotime(int64_t *sec, int64_t *nsec);
extern void *memcpy(void *, const void *, unsigned long);
extern void *memset(void *, int, unsigned long);

#include "vdso_image.h"   /* generated: vdso_image[], vdso_image_len */

#define VDSO_MAXPG 4

static uint64_t            vvar_phys;
static struct vdso_vvar   *vvar_k;            /* kernel-side view of vvar */
static uint64_t            vdso_phys[VDSO_MAXPG];
static int                 vdso_npg;
static uint32_t            cnt_mult, cnt_shift, cnt_valid;

static void vdso_update(void);

void vdso_init(void)
{
	void *p = buddy_alloc(0);
	if (!p) return;
	vvar_phys = (uint64_t)p;
	vvar_k = (struct vdso_vvar *)PHYS_TO_VIRT(vvar_phys);
	memset(vvar_k, 0, PAGE_SIZE);

	vdso_npg = (int)((vdso_image_len + PAGE_SIZE - 1) / PAGE_SIZE);
	if (vdso_npg > VDSO_MAXPG) vdso_npg = VDSO_MAXPG;
	for (int i = 0; i < vdso_npg; i++) {
		void *q = buddy_alloc(0);
		if (!q) { vvar_phys = 0; return; }
		vdso_phys[i] = (uint64_t)q;
		uint8_t *dst = (uint8_t *)PHYS_TO_VIRT(vdso_phys[i]);
		unsigned long off = (unsigned long)i * PAGE_SIZE;
		unsigned long n = vdso_image_len - off;
		if (n > PAGE_SIZE) n = PAGE_SIZE;
		memcpy(dst, vdso_image + off, n);
		if (n < PAGE_SIZE) memset(dst + n, 0, PAGE_SIZE - n);
	}

#if defined(__aarch64__)
	/* cntvct is user-readable (EL0 access enabled at cpu init): give the vDSO a
	 * mult/shift so delta_ns = (delta_cnt * mult) >> shift. */
	uint64_t frq;
	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frq));
	if (frq) { cnt_shift = 20; cnt_mult = (uint32_t)((1000000000ULL << 20) / frq); cnt_valid = 1; }
#else
	/* x86: no user-readable calibrated counter yet — serve the per-tick base
	 * (coarse but monotonic). cnt_valid stays 0. */
	cnt_valid = 0;
#endif

	vdso_update();
	tick_register(vdso_update);
}

/* Map the vDSO image (RX) and the vvar page (RO) into a process address space.
 * Called from exec after the new space is built. */
void vdso_map(vm_space_t *vm)
{
	if (!vvar_phys || !vm) return;
	vm_map(vm, VVAR_VADDR, vvar_phys, VM_USER | VM_READ | VM_NOFREE);
	for (int i = 0; i < vdso_npg; i++)
		vm_map(vm, VDSO_VADDR + (uint64_t)i * PAGE_SIZE, vdso_phys[i],
		       VM_USER | VM_READ | VM_EXEC | VM_NOFREE);
}

static inline uint64_t hw_counter(void)
{
#if defined(__aarch64__)
	uint64_t c; __asm__ volatile("mrs %0, cntvct_el0" : "=r"(c)); return c;
#else
	return 0;
#endif
}

/* Push a fresh vvar immediately — called when the kernel steps the clock
 * (settimeofday/clock_settime/adjtimex) so the vDSO doesn't serve a stale base
 * until the next tick. */
void vdso_time_changed(void) { vdso_update(); }

/* Refresh vvar under a seqlock (odd seq = update in progress). vdso_update runs
 * both as a timer-tick callback and from the clock-setting syscalls (via
 * vdso_time_changed); serialize the two writers under vdso_lock so a nested
 * (aarch64) or cross-CPU update can't drive seq back to even mid-write and let a
 * vDSO reader validate a torn mix of two updates. */
static spinlock_t vdso_lock = SPINLOCK_INIT;
static void vdso_update(void)
{
	if (!vvar_k) return;
	uint64_t f = spin_lock_irq(&vdso_lock);
	uint64_t cnt = hw_counter();
	int64_t ms, mn, rs, rn;
	ranix_monotime(&ms, &mn);
	ranix_walltime(&rs, &rn);

	vvar_k->seq++;                       /* → odd */
	__asm__ volatile("" ::: "memory");
	vvar_k->cnt_base  = cnt;
	vvar_k->cnt_mult  = cnt_mult;
	vvar_k->cnt_shift = cnt_shift;
	vvar_k->cnt_valid = cnt_valid;
	vvar_k->mono_sec  = ms; vvar_k->mono_nsec = mn;
	vvar_k->real_sec  = rs; vvar_k->real_nsec = rn;
	__asm__ volatile("" ::: "memory");
	vvar_k->seq++;                       /* → even */
	spin_unlock_irq(&vdso_lock, f);
}
