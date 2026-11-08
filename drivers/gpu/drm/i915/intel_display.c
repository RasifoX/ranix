// SPDX-License-Identifier: MIT
/*
 * Ranix: the Intel Gen9 (Skylake/Kaby Lake) display back end for KMS.
 *
 * What it does, in plain words: it finds the plane the firmware left on, reads
 * the mode and frame period from the pipe, and lets KMS show a client buffer by
 * pointing that plane at the buffer's pages instead of copying them. To do that
 * it owns a 128 MiB window at the top of the GGTT (the GPU's global page table),
 * which the render engine (ranix_gt.c) also uses for its own objects.
 *
 * No modeset: no PLLs, no link training; the firmware's mode stays.
 * Every GGTT entry in the window points at a page the allocator manages or at a
 * zeroed scratch page, never at 0 (as i915 does with its scratch page).
 * Registers: Intel Kaby Lake PRM Vol 2c (display), names as there.
 */
#include <kernel/mm/heap.h>
#include <kernel/mm/vm.h>
#include <kernel/mm/pmm.h>
#include <kernel/mm/buddy.h>
#include <kernel/lib/string.h>
#include <kernel/proc.h>
#include <kernel/kprintf.h>
#include <kernel/pci.h>
#include <kernel/cmdline.h>
#include <kernel/hal.h>
#include <kernel/kout.h>
#include "../drm_priv.h"
#include "intel_display.h"
#include "ranix_gt.h"

/* Gen9 display registers, pipe A; pipes B and C are +0x1000 each */
#define PIPEDSL(p)        (0x70000 + (p) * 0x1000)
#define PIPECONF(p)       (0x70008 + (p) * 0x1000)
#define PIPE_FRMCOUNT(p)  (0x70040 + (p) * 0x1000)
#define PIPE_CRC_CTL(p)   (0x60050 + (p) * 0x1000)
#define PIPE_CRC_RES(p)   (0x60064 + (p) * 0x1000)
#define PLANE_CTL(p)      (0x70180 + (p) * 0x1000)
#define PLANE_STRIDE(p)   (0x70188 + (p) * 0x1000)
#define PLANE_SURF(p)     (0x7019C + (p) * 0x1000)
#define PLANE_SURFLIVE(p) (0x701AC + (p) * 0x1000)
#define TRANS_HTOTAL(t)   ((t) + 0x00)
#define TRANS_HSYNC(t)    ((t) + 0x08)
#define TRANS_VTOTAL(t)   ((t) + 0x0C)
#define TRANS_VBLANK(t)   ((t) + 0x10)
#define TRANS_VSYNC(t)    ((t) + 0x14)
#define TRANS_EDP         0x6F000
#define TRANS_DDI_FUNC_CTL_EDP 0x6F400
#define GFX_FLSH_CNTL     0x101008
#define PLANE_CTL_ENABLE  (1u << 31)
#define GSM_OFFSET        0x800000           /* GGTT entries in BAR0 */
#define GGTT_SLOT         (256 * 1024)       /* linear scanout wants 256 KiB alignment */
#define GGTT_REGION       (128ull << 20)

static struct {
	volatile uint8_t *mmio;    /* BAR0: registers, GGTT at +8 MiB */
	int      pipe;
	int      hwcount;          /* the pipe frame counter moves */
	uint32_t vtotal, vblank_start, vdisplay;
	uint64_t line_ns;
	uint32_t hw_last;
	uint32_t fw_surf, fw_stride, fw_ctl;
	uint32_t want;             /* surface the plane was last pointed at */
	uint64_t ggtt_base;        /* our window */
	uint64_t gtt_entries;      /* the whole GGTT */
	uint8_t  slot_used[GGTT_REGION / GGTT_SLOT];
	uint64_t scratch;          /* zeroed page behind every unused entry */
	uint32_t refused;          /* maps refused: page not RAM */
	drm_fw_fb_t fb;            /* the firmware framebuffer, for the self-test */
} D;

static inline uint32_t rd(uint32_t r) { return *(volatile uint32_t *)(D.mmio + r); }
static inline void wr(uint32_t r, uint32_t v) { *(volatile uint32_t *)(D.mmio + r) = v; }
static volatile uint64_t *gsm(void) { return (volatile uint64_t *)(D.mmio + GSM_OFFSET); }
static uint64_t scratch_pte(void) { return D.scratch | 1; }    /* present, PAT 0 */

static void ggtt_flush(uint64_t first)
{
	(void)gsm()[first];                       /* post the entry writes */
	wr(GFX_FLSH_CNTL, 1);
	(void)rd(GFX_FLSH_CNTL);
}

/* ── for the render engine and the PPGTT ── */

volatile uint8_t *intel_mmio(void) { return D.mmio; }

uint64_t intel_scratch_page(void)
{
	if (!D.scratch) {
		void *pg = buddy_alloc(0);
		if (!pg) return 0;
		memset((void *)PHYS_TO_VIRT((uint64_t)pg), 0, PAGE_SIZE);
		D.scratch = (uint64_t)pg;
		drm_cache_flush((void *)PHYS_TO_VIRT(D.scratch), PAGE_SIZE);
	}
	return D.scratch;
}

/* RAM the page allocator manages: every object page comes from there */
int intel_phys_ok(uint64_t pa)
{
	if (!pa || (pa & (PAGE_SIZE - 1))) return 0;
	page_t *pg = paddr_to_page(pa);
	return pg && (pg->flags & PG_MANAGED);
}

int intel_ggtt_bind(drm_gem_t *o)
{
	if (o->ggtt) return 0;
	if (!D.ggtt_base || !D.scratch) return -ENODEV_;
	for (uint32_t i = 0; i < o->npages; i++)
		if (!intel_phys_ok(o->pages[i])) {
			if (D.refused++ < 8) kprintf("i915: refusing GGTT map of %lx (page %u of %u): not RAM\n",
			                             (unsigned long)o->pages[i], i, o->npages);
			return -EFAULT_;
		}
	uint32_t need = (uint32_t)((o->size + GGTT_SLOT - 1) / GGTT_SLOT), nslots = GGTT_REGION / GGTT_SLOT;
	for (uint32_t s = 0; s + need <= nslots; s++) {
		uint32_t k = 0;
		while (k < need && !D.slot_used[s + k]) k++;
		if (k < need) { s += k; continue; }
		for (k = 0; k < need; k++) D.slot_used[s + k] = 1;
		o->ggtt = D.ggtt_base + (uint64_t)s * GGTT_SLOT;
		o->ggtt_slots = need;
		uint64_t first = o->ggtt / PAGE_SIZE;
		for (uint32_t i = 0; i < o->npages; i++) gsm()[first + i] = o->pages[i] | 1;
		ggtt_flush(first);
		return 0;
	}
	return -ENOSPC_;
}

static void ggtt_unbind(drm_gem_t *o)
{
	if (!o->ggtt) return;
	/* the plane must have let go of it: SURFLIVE moves at the vblank after the
	 * switch away, so wait for that (bounded) before the entries go */
	for (int i = 0; i < 60; i++) {
		uint32_t live = rd(PLANE_SURFLIVE(D.pipe)) & ~0xFFFu;
		if (live < o->ggtt || live >= o->ggtt + o->size) break;
		drm_sleep_ms(1);
	}
	uint64_t first = o->ggtt / PAGE_SIZE;
	for (uint32_t i = 0; i < o->npages; i++) gsm()[first + i] = scratch_pte();
	ggtt_flush(first);
	uint32_t s = (uint32_t)((o->ggtt - D.ggtt_base) / GGTT_SLOT);
	for (uint32_t k = 0; k < o->ggtt_slots; k++) D.slot_used[s + k] = 0;
	o->ggtt = 0;
	o->ggtt_slots = 0;
}

/* Every GGTT entry, read only. In our window each must be the scratch page or
 * RAM; outside it (the firmware's part) count what is present and what is not
 * RAM (the boot picture's stolen memory is expected there). */
int intel_ggtt_audit(char *o, int max)
{
	kout_t k;
	kout_init(&k, o, (size_t)max);
	if (!D.mmio || !D.gtt_entries) { kout_s(&k, "audit: no GGTT\n"); return (int)k.len; }
	uint64_t wfirst = D.ggtt_base / PAGE_SIZE, wlast = D.ggtt_base ? wfirst + GGTT_REGION / PAGE_SIZE : 0;
	uint64_t w_scratch = 0, w_ram = 0, w_bad = 0, f_present = 0, f_notram = 0, f_zero = 0;
	for (uint64_t i = 0; i < D.gtt_entries; i++) {
		uint64_t e = gsm()[i], pa = e & 0x0000FFFFFFFFF000ULL;
		if (D.ggtt_base && i >= wfirst && i < wlast) {
			if (e == scratch_pte()) w_scratch++;
			else if ((e & 1) && intel_phys_ok(pa)) w_ram++;
			else w_bad++;
		} else if (!e) f_zero++;
		else if (e & 1) { f_present++; if (!intel_phys_ok(pa)) f_notram++; }
	}
	kout_s(&k, "audit: window scratch "); kout_u(&k, w_scratch);
	kout_s(&k, " ram "); kout_u(&k, w_ram);
	kout_s(&k, " BAD "); kout_u(&k, w_bad);
	kout_s(&k, " | firmware part present "); kout_u(&k, f_present);
	kout_s(&k, " not-ram "); kout_u(&k, f_notram);
	kout_s(&k, " zero "); kout_u(&k, f_zero);
	kout_s(&k, " | refused maps "); kout_u(&k, D.refused);
	kout_c(&k, '\n');
	return (int)k.len;
}

/* ── the display ops ── */

static int vblank(uint32_t *count, uint64_t *since_ns)
{
	if (!D.hwcount) return 0;
	*count = rd(PIPE_FRMCOUNT(D.pipe));
	uint32_t line = rd(PIPEDSL(D.pipe)) & 0x1FFF;
	*since_ns = (uint64_t)((line + D.vtotal - D.vblank_start) % D.vtotal) * D.line_ns;
	return 1;
}

/* return once the beam is in vertical blank (bounded): a copy started there outruns it */
static void wait_vblank_start(void)
{
	uint64_t give_up = drm_now_ns() + 50000000ULL;
	while (drm_now_ns() < give_up) {
		uint32_t line = rd(PIPEDSL(D.pipe)) & 0x1FFF;
		if (line >= D.vdisplay && line < D.vtotal) return;
		uint64_t left = (uint64_t)(D.vdisplay > line ? D.vdisplay - line : 0) * D.line_ns;
		if (left > 2000000) drm_sleep_ms(1);
		else cpu_relax();
	}
}

/* point the plane at surf; the hardware latches it at the next vblank */
static void plane_point(uint32_t surf, uint32_t stride)
{
	D.want = surf;
	wr(PLANE_STRIDE(D.pipe), stride);
	wr(PLANE_SURF(D.pipe), surf);             /* arms the update */
	(void)rd(PLANE_SURF(D.pipe));
}

static int flip(drm_gem_t *o, uint32_t pitch)
{
	if (pitch & 63 || pitch / 64 > 0x3FF) return -EINVAL_;   /* what the plane can take */
	int e = intel_ggtt_bind(o);
	if (e) return e;
	drm_gem_flush(o, 0, o->size);             /* the display does not snoop the CPU caches */
	plane_point((uint32_t)o->ggtt, pitch / 64);
	return 0;
}

static void flip_firmware(void) { plane_point(D.fw_surf & ~0xFFFu, D.fw_stride); }
static int  flip_landed(void)   { return (rd(PLANE_SURFLIVE(D.pipe)) & ~0xFFFu) == D.want; }
static void release(drm_gem_t *o) { ggtt_unbind(o); }

/* pipe CRC of what plane 1 sends out: the same image copied or flipped gives the
 * same value, which proves a flip shows the right pixels */
static uint32_t crc_now(void)
{
	if (!(rd(PIPE_CRC_CTL(D.pipe)) & (1u << 31))) wr(PIPE_CRC_CTL(D.pipe), 1u << 31);   /* source plane 1 */
	return rd(PIPE_CRC_RES(D.pipe));
}

static int debug(char *o, int max)
{
	kout_t k;
	kout_init(&k, o, (size_t)max);
	kout_s(&k, "ctl=");       kout_x(&k, rd(PLANE_CTL(D.pipe)));
	kout_s(&k, " surf=");     kout_x(&k, rd(PLANE_SURF(D.pipe)));
	kout_s(&k, " live=");     kout_x(&k, rd(PLANE_SURFLIVE(D.pipe)));
	kout_s(&k, " stride=");   kout_x(&k, rd(PLANE_STRIDE(D.pipe)));
	kout_s(&k, " frm=");      kout_x(&k, rd(PIPE_FRMCOUNT(D.pipe)));
	kout_s(&k, " base=");     kout_x(&k, D.ggtt_base);
	kout_s(&k, " pipeconf="); kout_x(&k, rd(PIPECONF(D.pipe)));
	kout_s(&k, " crc=");      kout_x(&k, crc_now());
	return (int)k.len;
}

static uint32_t crc_settled(void) { crc_now(); drm_sleep_ms(60); return crc_now(); }

static void plane_to(uint32_t surf)
{
	D.want = surf;
	wr(PLANE_SURF(D.pipe), surf);
	for (int i = 0; i < 60 && !flip_landed(); i++) drm_sleep_ms(1);
}

/* The GGTT window shows (a) the firmware's own pages, (b) a copy of them in RAM;
 * each is compared by pipe CRC with the firmware plane. KMS calls this only with
 * no client buffer on the plane and no flip in flight. */
static int selftest(void)
{
	if (!D.ggtt_base) return -ENODEV_;
	uint32_t fw = D.fw_surf & ~0xFFFu, npg = (uint32_t)((D.fb.pitch * (uint64_t)D.fb.h + PAGE_SIZE - 1) / PAGE_SIZE);
	uint64_t win = D.ggtt_base + GGTT_REGION - 16ull * 1024 * 1024;   /* last 16 MiB of our window */
	uint64_t first = win / PAGE_SIZE;
	uint32_t c0 = crc_settled();
	for (uint32_t i = 0; i < npg; i++) gsm()[first + i] = gsm()[fw / PAGE_SIZE + i];
	ggtt_flush(first);
	plane_to((uint32_t)win);
	uint32_t c1 = crc_settled();
	plane_to(fw);
	drm_gem_t *o = drm_gem_alloc((uint64_t)npg * PAGE_SIZE);
	uint32_t c2 = 0;
	if (o) {
		for (uint32_t i = 0; i < npg; i++) {
			uint64_t left = D.fb.size - (uint64_t)i * PAGE_SIZE;
			memcpy((void *)PHYS_TO_VIRT(o->pages[i]), D.fb.mem + (uint64_t)i * PAGE_SIZE, left < PAGE_SIZE ? left : PAGE_SIZE);
		}
		drm_gem_flush(o, 0, o->size);
		for (uint32_t i = 0; i < npg; i++) gsm()[first + i] = o->pages[i] | 1;
		ggtt_flush(first);
		plane_to((uint32_t)win);
		c2 = crc_settled();
		plane_to(fw);
		drm_gem_put(o);
	}
	for (uint32_t i = 0; i < npg; i++) gsm()[first + i] = scratch_pte();
	ggtt_flush(first);
	D.want = fw;
	kprintf("drm: selftest crc fw %08x, window->fw pages %08x, window->ram copy %08x\n", c0, c1, c2);
	return c0 == c1 && c1 == c2 ? 0 : -EIO_;
}

static void tick(void) { gt_tick(); }

static const drm_display_t intel_display = {
	.name = "i915", .desc = "Intel Graphics (Ranix)", .sysfs_driver = "pci/drivers/i915",
	.version_minor = 6,
	.conn_type = DRM_MODE_CONNECTOR_eDP, .enc_type = DRM_MODE_ENCODER_TMDS,
	.vblank = vblank, .wait_vblank_start = wait_vblank_start,
	.flip = flip, .flip_firmware = flip_firmware, .flip_landed = flip_landed,
	.release = release, .tick = tick, .debug = debug, .selftest = selftest,
	.driver = ranix_i915_init,
};

/* ── probe ── */

/* The firmware's mode, from the transcoder of the pipe whose plane is on. */
static int read_mode(struct drm_mode_modeinfo *m)
{
	uint32_t t = (rd(TRANS_DDI_FUNC_CTL_EDP) & (1u << 31)) ? TRANS_EDP : 0x60000 + (uint32_t)D.pipe * 0x1000;
	uint32_t ht = rd(TRANS_HTOTAL(t)), hs = rd(TRANS_HSYNC(t)), vt = rd(TRANS_VTOTAL(t)),
	         vb = rd(TRANS_VBLANK(t)), vs = rd(TRANS_VSYNC(t));
	if (!vt) return -1;
	D.vtotal = (vt >> 16) + 1;
	D.vblank_start = (vb & 0x1FFF) + 1;
	if (D.vblank_start < D.vdisplay || D.vblank_start >= D.vtotal) D.vblank_start = D.vdisplay;
	m->htotal = (uint16_t)((ht >> 16) + 1);
	m->hsync_start = (uint16_t)((hs & 0x1FFF) + 1);
	m->hsync_end = (uint16_t)((hs >> 16) + 1);
	m->vtotal = (uint16_t)D.vtotal;
	m->vsync_start = (uint16_t)((vs & 0x1FFF) + 1);
	m->vsync_end = (uint16_t)((vs >> 16) + 1);
	return 0;
}

/* frame period from the pipe frame counter: two edges, bounded to 80 ms */
static uint64_t measure_frame(void)
{
	uint32_t c1 = rd(PIPE_FRMCOUNT(D.pipe));
	uint64_t t0 = drm_now_ns(), e1 = 0;
	while (drm_now_ns() - t0 < 80000000ULL) {
		uint32_t c = rd(PIPE_FRMCOUNT(D.pipe));
		if (c != c1) {
			c1 = c;
			if (e1) { D.hw_last = c; return drm_now_ns() - e1; }
			e1 = drm_now_ns();
		}
		cpu_relax();
	}
	return 0;
}

/* GGTT size from GMCH_CTRL.GGMS; our window at its top, every entry on scratch */
static void ggtt_init(uint8_t b, uint8_t dv, uint8_t fn)
{
	uint32_t ggms = (pci_read16(b, dv, fn, 0x50) >> 6) & 3;
	uint64_t space = ggms ? ((1ULL << ggms) << 20) / 8 * PAGE_SIZE : 0;
	D.gtt_entries = space / PAGE_SIZE;
	if (space < 4 * GGTT_REGION || space > (8ULL << 30)) return;
	if ((D.fw_surf & ~0xFFFu) >= space - GGTT_REGION) return;   /* the firmware surface is up there */
	if (!intel_scratch_page()) return;
	D.ggtt_base = space - GGTT_REGION;
	uint64_t first = D.ggtt_base / PAGE_SIZE;
	for (uint64_t i = 0; i < GGTT_REGION / PAGE_SIZE; i++) gsm()[first + i] = scratch_pte();
	ggtt_flush(first);
}

const drm_display_t *intel_display_probe(const drm_fw_fb_t *fb, struct drm_mode_modeinfo *mode, uint64_t *frame_ns, int *can_flip)
{
	if (!drm_pci_found) return NULL;
	uint8_t b = drm_pci_bus, dv = drm_pci_dev, fn = drm_pci_fn;
	if (pci_read16(b, dv, fn, PCI_VENDOR_ID) != 0x8086) return NULL;
	uint16_t id = pci_read16(b, dv, fn, PCI_DEVICE_ID);
	uint8_t hi = (uint8_t)(id >> 8);
	if (hi != 0x19 && hi != 0x59 && hi != 0x3E && hi != 0x9B) return NULL;   /* Gen9 display */
	uint64_t bar = (pci_read32(b, dv, fn, PCI_BAR0) & ~0xFULL) | (uint64_t)pci_read32(b, dv, fn, PCI_BAR1) << 32;
	if (!bar) return NULL;
	D.mmio = ioremap(bar, 0x1000000, VM_READ | VM_WRITE | VM_NOCACHE | VM_DEVICE);
	if (!D.mmio) return NULL;
	D.fb = *fb;
	D.vdisplay = fb->h;
	D.pipe = -1;
	for (int p = 0; p < 3; p++)
		if (rd(PLANE_CTL(p)) & PLANE_CTL_ENABLE) { D.pipe = p; break; }
	if (D.pipe < 0) return NULL;
	D.fw_ctl = rd(PLANE_CTL(D.pipe));
	D.fw_surf = rd(PLANE_SURF(D.pipe));
	D.fw_stride = rd(PLANE_STRIDE(D.pipe));
	D.want = D.fw_surf & ~0xFFFu;
	if (read_mode(mode)) return NULL;
	uint64_t f = measure_frame();
	if (f) { D.hwcount = 1; *frame_ns = f; }
	D.line_ns = *frame_ns / D.vtotal;
	ggtt_init(b, dv, fn);
	*can_flip = D.ggtt_base && !cmdline_has("ranix.drmcopy");
	kprintf("drm: i915 %04x pipe %c plane ctl %08x surf %08x stride %u, vtotal %u, %u.%03u ms/frame, ggtt %lu MiB%s\n",
	        id, 'A' + D.pipe, D.fw_ctl, D.fw_surf, D.fw_stride, D.vtotal,
	        (unsigned)(*frame_ns / 1000000), (unsigned)(*frame_ns / 1000 % 1000),
	        (unsigned long)(D.gtt_entries * PAGE_SIZE >> 20), *can_flip ? ", flips" : "");
	return &intel_display;
}
