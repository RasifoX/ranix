/*
 * Ranix KMS: one CRTC, one encoder, one connector, a primary and a cursor plane,
 * all on the mode the firmware left running (no modeset: no PLLs, no link
 * training). Two ways to put a client framebuffer on the glass:
 *
 *  copy  - every target: the framebuffer is copied (and converted) into the
 *          firmware's linear framebuffer. A page flip waits for the beam to enter
 *          vertical blank before copying, and the copy outruns the beam, so a full
 *          frame lands without tearing. The cursor is drawn in software here.
 *  flip  - when a display back end can (i915/intel_display.c): the plane is
 *          pointed at the client buffer itself. Default where available;
 *          "ranix.drmcopy" or "0" into /sys/class/drm/ranix_flip turns it off.
 *          A visible cursor or a buffer of another size or format is copied.
 *
 * Vblank: the back end's frame counter, sampled from the 1 ms tick; without
 * one a 60 Hz clock.
 */
#include <kernel/vfs.h>
#include <kernel/fb.h>
#include <kernel/mm/heap.h>
#include <kernel/mm/vm.h>
#include <kernel/mm/pmm.h>
#include <kernel/lib/string.h>
#include <kernel/proc.h>
#include <kernel/hal.h>
#include <kernel/kprintf.h>
#include <kernel/pci.h>
#include <kernel/sync.h>
#include <kernel/kout.h>
#include <kernel/tty/fb_tty.h>
#include "drm_priv.h"


/* object ids (any non-zero u32; separate ranges keep them recognisable) */
#define ID_PLANE_PRIMARY 31
#define ID_PLANE_CURSOR  32
#define ID_CRTC          41
#define ID_ENCODER       42
#define ID_CONNECTOR     43
#define PROP_DPMS        1
#define PROP_TYPE        2
#define PROP_NONDESKTOP  3
#define FB_ID_BASE       1000
#define BLOB_ID_BASE     50000

typedef struct drm_blob {
	uint32_t id, len;
	drm_filep_t *owner;
	struct drm_blob *next;
	uint8_t data[];
} drm_blob_t;

static struct {
	int ok;
	uint32_t w, h;
	uint8_t *fbmem;            /* firmware framebuffer, write-combining */
	uint32_t fbpitch;
	int      fb_bgr;           /* firmware fb stores R in the low byte */
	uint32_t *row;             /* one composed scanline */
	struct drm_mode_modeinfo mode;
	uint32_t conn_type, enc_type, mm_w, mm_h;

	const drm_display_t *hw;   /* display back end, or NULL */
	int      hwflip;           /* flips allowed */
	int      hw_scan;          /* plane shows a client buffer, not the firmware fb */
	int      hwcount;          /* back end has a frame counter */
	uint32_t hw_last;

	/* CRTC state */
	int       enabled;
	drm_fb_t *scan;
	uint32_t  cx, cy;
	uint32_t  dpms;
	drm_fb_t *fbs;
	uint32_t  next_fb;
	drm_blob_t *blobs;
	uint32_t  next_blob;

	/* cursor (software in copy mode) */
	drm_gem_t *cur;
	uint32_t   cur_w, cur_h, cur_pitch;
	int32_t    cur_x, cur_y;

	/* vblank and flips, under drm_ev_lock */
	uint64_t vbl_seq, vbl_ns, frame_ns, sw_base;
	drm_pending_t *pending;    /* waiting for a sequence */
	int       flip_pending;    /* hardware flip latched at next vblank */
	drm_pending_t *flip_ev;
	drm_fb_t *flip_old;        /* shown until the flip lands, released after */
	int       flip_done;
	uint64_t  flip_seq;        /* vblank that completed the last flip */

	/* the kernel text console keeps off the screen while a client shows a buffer */
	int       kd_taken, kd_prev;
} K;


static void console_off(void)
{
	if (K.kd_taken) return;
	K.kd_prev = fb_tty_get_graphics();
	fb_tty_set_graphics(1);
	K.kd_taken = 1;
}

static void console_back(void)
{
	if (!K.kd_taken) return;
	K.kd_taken = 0;
	if (!K.kd_prev) fb_tty_set_graphics(0);    /* text mode before: it repaints itself */
}

/* state for /sys/class/drm/ranix_flip (after the 0/1 line) */
const drm_display_t *drm_display(void) { return K.hw; }

int drm_kms_debug(char *o)
{
	kout_t k;
	kout_init(&k, o, 400);
	kout_s(&k, "scan="); kout_u(&k, (uint64_t)K.hw_scan);
	kout_s(&k, " pend="); kout_u(&k, (uint64_t)K.flip_pending);
	kout_s(&k, " seq="); kout_u(&k, K.vbl_seq);
	if (K.hw && K.hw->debug) { kout_c(&k, ' '); k.len += (size_t)K.hw->debug(o + k.len, (int)(k.cap - k.len)); }
	kout_c(&k, '\n');
	return (int)k.len;
}
/* /sys/class/drm/ranix_flip: flip mode on (1) or off (0) at run time */
int drm_kms_hwflip(int set)
{
	if (set < 0) return K.hwflip;
	if (set && (!K.hw || !K.hw->flip)) return -ENODEV_;
	K.hwflip = set ? 1 : 0;
	return K.hwflip;
}
int drm_kms_mode_size(uint32_t *w, uint32_t *h) { *w = K.w; *h = K.h; return K.ok; }
int drm_kms_connector_status(void) { return K.ok; }
uint32_t drm_kms_dpms(void) { return K.dpms; }


/* ── vblank clock ─────────────────────────────────────────────── */

/* under drm_ev_lock: bring vbl_seq/vbl_ns up to date */
static void vbl_update_locked(void)
{
	uint64_t now = drm_now_ns();
	uint32_t hw; uint64_t since;
	if (K.hwcount && K.hw->vblank(&hw, &since)) {
		uint32_t d = hw - K.hw_last;
		if (d) {
			K.hw_last = hw;
			K.vbl_seq += d;
			K.vbl_ns = now > since ? now - since : now;   /* when this vblank began */
		}
	} else {
		uint64_t s = now / K.frame_ns;
		if (s != K.sw_base + K.vbl_seq) {
			K.vbl_seq = s - K.sw_base;
			K.vbl_ns = s * K.frame_ns;
		}
	}
}

static uint64_t vbl_now(uint64_t *ns)
{
	uint64_t f = spin_lock_irq(&drm_ev_lock);
	vbl_update_locked();
	uint64_t s = K.vbl_seq;
	if (ns) *ns = K.vbl_ns;
	spin_unlock_irq(&drm_ev_lock, f);
	return s;
}

static void stamp(drm_pending_t *p, uint64_t seq, uint64_t ns)
{
	if (p->kind == DRM_EVENT_CRTC_SEQUENCE) {
		p->ev.seq.time_ns = (int64_t)ns;
		p->ev.seq.sequence = seq;
	} else {
		p->ev.vbl.tv_sec  = (uint32_t)(ns / 1000000000ULL);
		p->ev.vbl.tv_usec = (uint32_t)(ns % 1000000000ULL / 1000);
		p->ev.vbl.sequence = (uint32_t)seq;
		p->ev.vbl.crtc_id = ID_CRTC;
	}
}

/* 1 ms tick on CPU0, interrupt context */
void drm_kms_tick(void)
{
	if (!K.ok) return;
	if (K.hw && K.hw->tick) K.hw->tick();
	uint64_t f = spin_lock_irq(&drm_ev_lock);
	if (!K.pending && !K.flip_pending) { spin_unlock_irq(&drm_ev_lock, f); return; }
	vbl_update_locked();
	/* at most one flip per vblank: a flip written during a vblank can latch before
	 * the frame counter moves, and two completions must not share a sequence */
	if (K.flip_pending && K.vbl_seq > K.flip_seq && K.hw->flip_landed()) {
		K.flip_seq = K.vbl_seq;
		K.flip_pending = 0;
		K.flip_done = 1;
		if (K.flip_ev) { stamp(K.flip_ev, K.vbl_seq, K.vbl_ns); drm_event_deliver(K.flip_ev); K.flip_ev = NULL; }
	}
	drm_pending_t **pp = &K.pending;
	while (*pp) {
		drm_pending_t *p = *pp;
		if (p->target <= K.vbl_seq) {
			*pp = p->next;
			stamp(p, K.vbl_seq, K.vbl_ns);
			drm_event_deliver(p);
		} else pp = &p->next;
	}
	spin_unlock_irq(&drm_ev_lock, f);
}


void drm_kms_gem_release(drm_gem_t *o)
{
	if (K.hw && K.hw->release) K.hw->release(o);
}

/* can the plane, as the firmware programmed it, show this framebuffer directly */
static int hw_eligible(drm_fb_t *fb)
{
	if (!K.hwflip || K.cur) return 0;
	if (fb->format != DRM_FORMAT_XRGB8888 && fb->format != DRM_FORMAT_ARGB8888) return 0;
	if (fb->width != K.w || fb->height != K.h || K.cx || K.cy || fb->offset) return 0;
	return 1;
}

static void hw_restore_fw(void)
{
	if (!K.hw_scan) return;
	K.hw->flip_firmware();
	K.hw_scan = 0;
}

/* "t" into ranix_flip: the back end's own check that a flip shows the right pixels */
int drm_kms_selftest(void)
{
	if (!K.hw || !K.hw->selftest || K.hw_scan || K.flip_pending) return -EBUSY_;
	return K.hw->selftest();
}

/* ── copy presentation ────────────────────────────────────────── */

static void fill_row(uint32_t *row, int x1, int x2, int y)
{
	drm_fb_t *fb = K.scan;
	if (!fb || !K.enabled) { memset(row + x1, 0, (size_t)(x2 - x1) * 4); return; }
	int64_t sy = (int64_t)y + K.cy;
	int64_t sx1 = (int64_t)x1 + K.cx, sx2 = (int64_t)x2 + K.cx;
	if (sy >= fb->height || sx1 >= fb->width) { memset(row + x1, 0, (size_t)(x2 - x1) * 4); return; }
	if (sx2 > fb->width) {
		int keep = (int)(fb->width - sx1);
		memset(row + x1 + keep, 0, (size_t)(x2 - x1 - keep) * 4);
		sx2 = fb->width;
	}
	uint64_t off = fb->offset + (uint64_t)sy * fb->pitch + (uint64_t)sx1 * fb->cpp;
	uint64_t bytes = (uint64_t)(sx2 - sx1) * fb->cpp;
	uint32_t *out = row + x1;
	if (fb->cpp == 4) {
		uint8_t *dst = (uint8_t *)out;
		while (bytes) {
			uint64_t av;
			void *src = drm_gem_vaddr(fb->obj, off, &av);
			if (!src) { memset(dst, 0, bytes); break; }
			uint64_t n = av < bytes ? av : bytes;
			memcpy(dst, src, n);
			dst += n; off += n; bytes -= n;
		}
		int np = (int)(sx2 - sx1);
		if (fb->format == DRM_FORMAT_XBGR8888 || fb->format == DRM_FORMAT_ABGR8888)
			for (int i = 0; i < np; i++) {
				uint32_t v = out[i];
				out[i] = (v & 0xFF00FF00u) | ((v >> 16) & 0xFF) | ((v & 0xFF) << 16);
			}
	} else {                                   /* RGB565 */
		int np = (int)(sx2 - sx1);
		for (int i = 0; i < np; i++, off += 2) {
			uint16_t *s = drm_gem_vaddr(fb->obj, off, NULL);
			uint16_t v = s ? *s : 0;
			uint32_t r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
			out[i] = ((r << 3 | r >> 2) << 16) | ((g << 2 | g >> 4) << 8) | (b << 3 | b >> 2);
		}
	}
}

static void cursor_row(uint32_t *row, int x1, int x2, int y)
{
	if (!K.cur || y < K.cur_y || y >= K.cur_y + (int)K.cur_h) return;
	int a1 = K.cur_x > x1 ? K.cur_x : x1;
	int a2 = K.cur_x + (int)K.cur_w < x2 ? K.cur_x + (int)K.cur_w : x2;
	if (a1 >= a2) return;
	uint64_t base = (uint64_t)(y - K.cur_y) * K.cur_pitch;
	for (int x = a1; x < a2; x++) {
		uint32_t *sp = drm_gem_vaddr(K.cur, base + (uint64_t)(x - K.cur_x) * 4, NULL);
		if (!sp) continue;
		uint32_t s = *sp, a = s >> 24;
		if (!a) continue;
		if (a == 255) { row[x] = s; continue; }
		uint32_t d = row[x], ia = 255 - a, o = 0;
		for (int sh = 0; sh < 24; sh += 8) {        /* premultiplied: s + d * (1 - a) */
			uint32_t c = ((s >> sh) & 0xFF) + (((d >> sh) & 0xFF) * ia + 127) / 255;
			o |= (c > 255 ? 255 : c) << sh;
		}
		row[x] = o;
	}
}

static void present(int x1, int y1, int x2, int y2)
{
	if (x1 < 0) x1 = 0;
	if (y1 < 0) y1 = 0;
	if (x2 > (int)K.w) x2 = (int)K.w;
	if (y2 > (int)K.h) y2 = (int)K.h;
	if (x1 >= x2 || y1 >= y2) return;
	for (int y = y1; y < y2; y++) {
		fill_row(K.row, x1, x2, y);
		cursor_row(K.row, x1, x2, y);
		if (K.fb_bgr)
			for (int x = x1; x < x2; x++) {
				uint32_t v = K.row[x];
				K.row[x] = (v & 0xFF00FF00u) | ((v >> 16) & 0xFF) | ((v & 0xFF) << 16);
			}
		memcpy(K.fbmem + (uint64_t)y * K.fbpitch + (uint64_t)x1 * 4, K.row + x1, (size_t)(x2 - x1) * 4);
	}
}

static void present_all(void) { present(0, 0, (int)K.w, (int)K.h); }

/* Wait for the beam to enter vertical blank; a copy started there outruns it. */
static void wait_vblank_start(void)
{
	if (K.hw && K.hw->wait_vblank_start) K.hw->wait_vblank_start();
}

/* ── showing a framebuffer ────────────────────────────────────── */

static void fb_put(drm_fb_t *fb)
{
	if (!fb || --fb->refcount) return;
	drm_gem_put(fb->obj);
	kfree(fb);
}

static void reap_flip(void)
{
	uint64_t f = spin_lock_irq(&drm_ev_lock);
	drm_fb_t *old = K.flip_done ? K.flip_old : NULL;
	if (old) { K.flip_old = NULL; K.flip_done = 0; }
	spin_unlock_irq(&drm_ev_lock, f);
	fb_put(old);
}

/* wait (bounded) for a hardware flip in flight */
static void flip_settle(void)
{
	for (int i = 0; i < 60 && K.flip_pending; i++) drm_sleep_ms(1);
	reap_flip();
}

/* Make fb (already referenced by the caller for the CRTC) the scanout. With ev,
 * deliver it when the new image is on screen. vsync: wait for vblank (copy). */
static int show(drm_fb_t *fb, drm_pending_t *ev, int vsync)
{
	drm_fb_t *old = K.scan;
	K.scan = fb;
	if (fb) console_off();
	/* bind and flush outside the lock (a whole buffer's cache lines); the flip
	 * counts as pending from here, and the tick completes it once it lands */
	if (fb && K.enabled && hw_eligible(fb) && K.hw->flip(fb->obj, fb->pitch) == 0) {
		uint64_t f = spin_lock_irq(&drm_ev_lock);
		K.flip_ev = ev;
		K.flip_old = old;
		K.flip_done = 0;
		K.flip_pending = 1;
		K.hw_scan = 1;
		spin_unlock_irq(&drm_ev_lock, f);
		return 0;
	}
	if (vsync) wait_vblank_start();
	present_all();
	if (K.hw_scan) {
		/* back to the firmware buffer, now holding this image */
		uint64_t f = spin_lock_irq(&drm_ev_lock);
		K.flip_ev = ev;
		K.flip_old = old;
		K.flip_done = 0;
		K.flip_pending = 1;
		hw_restore_fw();
		spin_unlock_irq(&drm_ev_lock, f);
		return 0;
	}
	if (ev) {
		uint64_t f = spin_lock_irq(&drm_ev_lock);
		vbl_update_locked();
		if (vsync && K.hwcount) {
			/* copied right after this vblank began: it is on screen now */
			stamp(ev, K.vbl_seq, K.vbl_ns);
			drm_event_deliver(ev);
		} else {
			/* no beam to race: complete at the next vblank, which paces the client */
			ev->target = K.vbl_seq + 1;
			ev->next = K.pending;
			K.pending = ev;
		}
		spin_unlock_irq(&drm_ev_lock, f);
	}
	fb_put(old);
	return 0;
}

/* turn the CRTC off. blank: paint black (an explicit disable), else leave the
 * last image (its owner went away). */
static void crtc_disable(int blank)
{
	flip_settle();
	drm_fb_t *old = K.scan;
	K.scan = NULL;
	if (K.hw_scan) {
		/* the firmware buffer gets the last image, then the plane goes back to it */
		if (old && !blank) { K.scan = old; K.hw_scan = 0; present_all(); K.scan = NULL; K.hw_scan = 1; }
		hw_restore_fw();
		for (int i = 0; i < 40 && !K.hw->flip_landed(); i++) drm_sleep_ms(1);
	}
	K.enabled = 0;
	if (blank) present_all();
	console_back();
	fb_put(old);
}

/* ── lookups ──────────────────────────────────────────────────── */

static drm_fb_t *fb_lookup(uint32_t id)
{
	for (drm_fb_t *f = K.fbs; f; f = f->next) if (f->id == id) return f;
	return NULL;
}

static int put_ids(uint64_t uptr, uint32_t ucount, const uint32_t *ids, uint32_t n)
{
	if (ucount >= n && n && uptr && copy_to_user((void *)(uintptr_t)uptr, ids, n * 4)) return -EFAULT_;
	return 0;
}

/* ── KMS ioctls ───────────────────────────────────────────────── */

int drm_ioctl_getresources(drm_filep_t *fp, void *d)
{
	struct drm_mode_card_res *r = d;
	uint32_t n = 0;
	for (drm_fb_t *f = K.fbs; f; f = f->next) if (f->owner == fp) n++;
	if (r->count_fbs >= n && n && r->fb_id_ptr) {
		uint32_t i = 0;
		for (drm_fb_t *f = K.fbs; f; f = f->next)
			if (f->owner == fp) {
				if (copy_to_user((void *)(uintptr_t)(r->fb_id_ptr + 4ULL * i), &f->id, 4)) return -EFAULT_;
				i++;
			}
	}
	r->count_fbs = n;
	uint32_t c = ID_CRTC, e = ID_ENCODER, k = ID_CONNECTOR;
	int err;
	if ((err = put_ids(r->crtc_id_ptr, r->count_crtcs, &c, 1))) return err;
	if ((err = put_ids(r->encoder_id_ptr, r->count_encoders, &e, 1))) return err;
	if ((err = put_ids(r->connector_id_ptr, r->count_connectors, &k, 1))) return err;
	r->count_crtcs = r->count_encoders = r->count_connectors = 1;
	r->min_width = r->min_height = 1;
	r->max_width = r->max_height = 8192;
	return 0;
}

int drm_ioctl_getcrtc(drm_filep_t *fp, void *d)
{
	(void)fp;
	struct drm_mode_crtc *c = d;
	if (c->crtc_id != ID_CRTC) return -ENOENT_;
	c->fb_id = K.enabled && K.scan ? K.scan->id : 0;
	c->x = K.cx; c->y = K.cy;
	c->gamma_size = 0;
	c->mode_valid = K.enabled;
	if (K.enabled) c->mode = K.mode; else memset(&c->mode, 0, sizeof(c->mode));
	return 0;
}

int drm_ioctl_setcrtc(drm_filep_t *fp, void *d)
{
	(void)fp;
	struct drm_mode_crtc *c = d;
	if (c->crtc_id != ID_CRTC) return -ENOENT_;
	reap_flip();
	if (!c->mode_valid) {
		if (c->fb_id || c->count_connectors) return -EINVAL_;
		crtc_disable(1);
		return 0;
	}
	if (c->mode.hdisplay != K.w || c->mode.vdisplay != K.h) return -EINVAL_;   /* no modeset (yet) */
	if (c->count_connectors > 1) return -EINVAL_;
	if (c->count_connectors) {
		uint32_t id;
		if (copy_from_user(&id, (void *)(uintptr_t)c->set_connectors_ptr, 4)) return -EFAULT_;
		if (id != ID_CONNECTOR) return -ENOENT_;
	}
	if (c->fb_id == 0) {
		/* the state GETCRTC reported before any client: the firmware picture with no
		 * framebuffer object. Clients (glmark2) put it back on exit; the console
		 * returns to the screen. */
		crtc_disable(0);
		K.enabled = 1;
		return 0;
	}
	drm_fb_t *fb;
	if (c->fb_id == 0xFFFFFFFFu) fb = K.scan;
	else fb = fb_lookup(c->fb_id);
	if (!fb) return -ENOENT_;
	if ((uint64_t)c->x + K.w > fb->width || (uint64_t)c->y + K.h > fb->height) return -ENOSPC_;
	flip_settle();
	fb->refcount++;
	K.cx = c->x; K.cy = c->y;
	K.enabled = 1;
	int e = show(fb, NULL, 0);
	flip_settle();                         /* legacy SETCRTC returns with the image on screen */
	return e;
}

static int cursor_set(uint32_t flags, uint32_t crtc, int32_t x, int32_t y, uint32_t w, uint32_t h,
                      uint32_t handle, drm_filep_t *fp)
{
	if (crtc != ID_CRTC) return -ENOENT_;
	if (!(flags & (DRM_MODE_CURSOR_BO | DRM_MODE_CURSOR_MOVE)) || (flags & ~3u)) return -EINVAL_;
	int ox = K.cur_x, oy = K.cur_y, ow = (int)K.cur_w, oh = (int)K.cur_h, was = K.cur != NULL;
	if (flags & DRM_MODE_CURSOR_BO) {
		drm_gem_t *o = NULL;
		if (handle) {
			if (!w || !h || w > 256 || h > 256) return -EINVAL_;
			o = drm_gem_lookup(fp, handle);
			if (!o) return -ENOENT_;
			if ((uint64_t)w * h * 4 > o->size) return -EINVAL_;
			drm_gem_get(o);
		}
		drm_gem_put(K.cur);
		K.cur = o;
		K.cur_w = o ? w : 0; K.cur_h = o ? h : 0; K.cur_pitch = w * 4;
	}
	if (flags & DRM_MODE_CURSOR_MOVE) { K.cur_x = x; K.cur_y = y; }
	if (!K.enabled) return 0;
	if (K.hw_scan && K.cur) {                  /* the cursor is drawn by copying: leave flip mode */
		flip_settle();
		drm_fb_t *s = K.scan;
		if (s) s->refcount++;
		int e = show(s, NULL, 1);
		flip_settle();
		return e;
	}
	if (!K.hw_scan) {
		if (was) present(ox, oy, ox + ow, oy + oh);
		if (K.cur) present(K.cur_x, K.cur_y, K.cur_x + (int)K.cur_w, K.cur_y + (int)K.cur_h);
		if (!K.cur && was && K.scan && hw_eligible(K.scan)) {   /* cursor gone: flip mode again */
			K.scan->refcount++;
			flip_settle();
			show(K.scan, NULL, 0);
			flip_settle();
		}
	}
	return 0;
}

int drm_ioctl_cursor(drm_filep_t *fp, void *d)
{
	struct drm_mode_cursor *c = d;
	return cursor_set(c->flags, c->crtc_id, c->x, c->y, c->width, c->height, c->handle, fp);
}

int drm_ioctl_cursor2(drm_filep_t *fp, void *d)
{
	struct drm_mode_cursor2 *c = d;
	return cursor_set(c->flags, c->crtc_id, c->x, c->y, c->width, c->height, c->handle, fp);
}

int drm_ioctl_getgamma(drm_filep_t *fp, void *d)
{
	(void)fp;
	struct drm_mode_crtc_lut *l = d;
	if (l->crtc_id != ID_CRTC) return -ENOENT_;
	return l->gamma_size ? -EINVAL_ : 0;
}

int drm_ioctl_setgamma(drm_filep_t *fp, void *d) { return drm_ioctl_getgamma(fp, d); }

int drm_ioctl_getencoder(drm_filep_t *fp, void *d)
{
	(void)fp;
	struct drm_mode_get_encoder *e = d;
	if (e->encoder_id != ID_ENCODER) return -ENOENT_;
	e->encoder_type = K.enc_type;
	e->crtc_id = ID_CRTC;
	e->possible_crtcs = 1;
	e->possible_clones = 0;
	return 0;
}

int drm_ioctl_getconnector(drm_filep_t *fp, void *d)
{
	(void)fp;
	struct drm_mode_get_connector *c = d;
	if (c->connector_id != ID_CONNECTOR) return -ENOENT_;
	uint32_t enc = ID_ENCODER;
	int e;
	if ((e = put_ids(c->encoders_ptr, c->count_encoders, &enc, 1))) return e;
	if (c->count_modes >= 1 && c->modes_ptr &&
	    copy_to_user((void *)(uintptr_t)c->modes_ptr, &K.mode, sizeof(K.mode))) return -EFAULT_;
	uint32_t props[2] = { PROP_DPMS, PROP_NONDESKTOP };
	uint64_t vals[2] = { K.dpms, 0 };
	if (c->count_props >= 2) {
		if (c->props_ptr && copy_to_user((void *)(uintptr_t)c->props_ptr, props, sizeof(props))) return -EFAULT_;
		if (c->prop_values_ptr && copy_to_user((void *)(uintptr_t)c->prop_values_ptr, vals, sizeof(vals))) return -EFAULT_;
	}
	c->count_encoders = 1;
	c->count_modes = 1;
	c->count_props = 2;
	c->encoder_id = ID_ENCODER;
	c->connector_type = K.conn_type;
	c->connector_type_id = 1;
	c->connection = DRM_MODE_CONNECTED;
	c->mm_width = K.mm_w;
	c->mm_height = K.mm_h;
	c->subpixel = DRM_MODE_SUBPIXEL_UNKNOWN;
	c->pad = 0;
	return 0;
}

static const struct { uint32_t id, flags; const char *name; } props_tab[] = {
	{ PROP_DPMS,       DRM_MODE_PROP_ENUM, "DPMS" },
	{ PROP_TYPE,       DRM_MODE_PROP_ENUM | DRM_MODE_PROP_IMMUTABLE, "type" },
	{ PROP_NONDESKTOP, DRM_MODE_PROP_RANGE | DRM_MODE_PROP_IMMUTABLE, "non-desktop" },
};
static const char *const dpms_names[] = { "On", "Standby", "Suspend", "Off" };
static const char *const type_names[] = { "Overlay", "Primary", "Cursor" };

int drm_ioctl_getproperty(drm_filep_t *fp, void *d)
{
	(void)fp;
	struct drm_mode_get_property *p = d;
	unsigned i = 0;
	while (i < 3 && props_tab[i].id != p->prop_id) i++;
	if (i == 3) return -ENOENT_;
	memset(p->name, 0, sizeof(p->name));
	strcpy(p->name, props_tab[i].name);
	p->flags = props_tab[i].flags;
	if (p->prop_id == PROP_NONDESKTOP) {
		uint64_t mm[2] = { 0, 1 };
		if (p->count_values >= 2 && p->values_ptr &&
		    copy_to_user((void *)(uintptr_t)p->values_ptr, mm, sizeof(mm))) return -EFAULT_;
		p->count_values = 2;
		p->count_enum_blobs = 0;
		return 0;
	}
	const char *const *names = p->prop_id == PROP_DPMS ? dpms_names : type_names;
	uint32_t n = p->prop_id == PROP_DPMS ? 4 : 3;
	if (p->count_values >= n && p->values_ptr)
		for (uint32_t k = 0; k < n; k++) {
			uint64_t v = k;
			if (copy_to_user((void *)(uintptr_t)(p->values_ptr + 8ULL * k), &v, 8)) return -EFAULT_;
		}
	if (p->count_enum_blobs >= n && p->enum_blob_ptr)
		for (uint32_t k = 0; k < n; k++) {
			struct drm_mode_property_enum en;
			memset(&en, 0, sizeof(en));
			en.value = k;
			strcpy(en.name, names[k]);
			if (copy_to_user((void *)(uintptr_t)(p->enum_blob_ptr + sizeof(en) * k), &en, sizeof(en))) return -EFAULT_;
		}
	p->count_values = n;
	p->count_enum_blobs = n;
	return 0;
}

static int set_prop(uint32_t obj, uint32_t type, uint32_t prop, uint64_t v)
{
	if (obj == ID_CONNECTOR && (type == DRM_MODE_OBJECT_CONNECTOR || type == DRM_MODE_OBJECT_ANY)) {
		if (prop == PROP_DPMS) { if (v > 3) return -EINVAL_; K.dpms = (uint32_t)v; return 0; }
		return -EINVAL_;
	}
	if (obj == ID_CRTC || obj == ID_PLANE_PRIMARY || obj == ID_PLANE_CURSOR || obj == ID_ENCODER) return -EINVAL_;
	return -ENOENT_;
}

int drm_ioctl_setproperty(drm_filep_t *fp, void *d)
{
	(void)fp;
	struct drm_mode_connector_set_property *s = d;
	return set_prop(s->connector_id, DRM_MODE_OBJECT_CONNECTOR, s->prop_id, s->value);
}

int drm_ioctl_obj_setproperty(drm_filep_t *fp, void *d)
{
	(void)fp;
	struct drm_mode_obj_set_property *s = d;
	return set_prop(s->obj_id, s->obj_type, s->prop_id, s->value);
}

int drm_ioctl_obj_getproperties(drm_filep_t *fp, void *d)
{
	(void)fp;
	struct drm_mode_obj_get_properties *g = d;
	uint32_t ids[2]; uint64_t vals[2]; uint32_t n = 0, t = g->obj_type;
	int any = t == DRM_MODE_OBJECT_ANY;
	if (g->obj_id == ID_CONNECTOR && (any || t == DRM_MODE_OBJECT_CONNECTOR)) {
		ids[0] = PROP_DPMS; vals[0] = K.dpms; ids[1] = PROP_NONDESKTOP; vals[1] = 0; n = 2;
	} else if ((g->obj_id == ID_PLANE_PRIMARY || g->obj_id == ID_PLANE_CURSOR) && (any || t == DRM_MODE_OBJECT_PLANE)) {
		ids[0] = PROP_TYPE;
		vals[0] = g->obj_id == ID_PLANE_PRIMARY ? DRM_PLANE_TYPE_PRIMARY : DRM_PLANE_TYPE_CURSOR;
		n = 1;
	} else if (g->obj_id == ID_CRTC && (any || t == DRM_MODE_OBJECT_CRTC)) {
		n = 0;
	} else if (g->obj_id == ID_ENCODER || fb_lookup(g->obj_id)) {
		return -EINVAL_;                  /* objects without properties */
	} else return -ENOENT_;
	if (g->count_props >= n && n) {
		if (g->props_ptr && copy_to_user((void *)(uintptr_t)g->props_ptr, ids, n * 4)) return -EFAULT_;
		if (g->prop_values_ptr && copy_to_user((void *)(uintptr_t)g->prop_values_ptr, vals, n * 8)) return -EFAULT_;
	}
	g->count_props = n;
	return 0;
}

int drm_ioctl_getpropblob(drm_filep_t *fp, void *d)
{
	(void)fp;
	struct drm_mode_get_blob *g = d;
	for (drm_blob_t *b = K.blobs; b; b = b->next)
		if (b->id == g->blob_id) {
			if (g->length >= b->len && g->data &&
			    copy_to_user((void *)(uintptr_t)g->data, b->data, b->len)) return -EFAULT_;
			g->length = b->len;
			return 0;
		}
	return -ENOENT_;
}

int drm_ioctl_createpropblob(drm_filep_t *fp, void *d)
{
	struct drm_mode_create_blob *c = d;
	if (!c->length || c->length > 65536) return -EINVAL_;
	drm_blob_t *b = kmalloc(sizeof(*b) + c->length);
	if (!b) return -ENOMEM_;
	if (copy_from_user(b->data, (void *)(uintptr_t)c->data, c->length)) { kfree(b); return -EFAULT_; }
	b->id = BLOB_ID_BASE + K.next_blob++;
	b->len = c->length;
	b->owner = fp;
	b->next = K.blobs;
	K.blobs = b;
	c->blob_id = b->id;
	return 0;
}

int drm_ioctl_destroypropblob(drm_filep_t *fp, void *d)
{
	struct drm_mode_destroy_blob *x = d;
	for (drm_blob_t **pp = &K.blobs; *pp; pp = &(*pp)->next)
		if ((*pp)->id == x->blob_id) {
			if ((*pp)->owner != fp) return -EPERM_;
			drm_blob_t *b = *pp;
			*pp = b->next;
			kfree(b);
			return 0;
		}
	return -ENOENT_;
}

static uint32_t fmt_cpp(uint32_t f)
{
	switch (f) {
	case DRM_FORMAT_XRGB8888: case DRM_FORMAT_ARGB8888:
	case DRM_FORMAT_XBGR8888: case DRM_FORMAT_ABGR8888: return 4;
	case DRM_FORMAT_RGB565: return 2;
	}
	return 0;
}

static int fb_create(drm_filep_t *fp, uint32_t w, uint32_t h, uint32_t fmt, uint32_t handle,
                     uint32_t pitch, uint32_t offset, uint32_t *id)
{
	uint32_t cpp = fmt_cpp(fmt);
	if (!cpp) return -EINVAL_;
	if (!w || !h || w > 8192 || h > 8192) return -EINVAL_;
	if (pitch < w * cpp || (pitch & (cpp - 1))) return -EINVAL_;
	drm_gem_t *o = drm_gem_lookup(fp, handle);
	if (!o) return -ENOENT_;
	if ((uint64_t)offset + (uint64_t)pitch * (h - 1) + (uint64_t)w * cpp > o->size) return -EINVAL_;
	drm_fb_t *fb = kmalloc(sizeof(*fb));
	if (!fb) return -ENOMEM_;
	memset(fb, 0, sizeof(*fb));
	fb->id = FB_ID_BASE + K.next_fb++;
	fb->refcount = 1;
	fb->owner = fp;
	fb->obj = o;
	drm_gem_get(o);
	fb->width = w; fb->height = h; fb->pitch = pitch; fb->offset = offset;
	fb->format = fmt; fb->cpp = cpp;
	fb->next = K.fbs;
	K.fbs = fb;
	*id = fb->id;
	return 0;
}

int drm_ioctl_addfb(drm_filep_t *fp, void *d)
{
	struct drm_mode_fb_cmd *c = d;
	uint32_t fmt;
	if (c->bpp == 32 && c->depth == 24) fmt = DRM_FORMAT_XRGB8888;
	else if (c->bpp == 32 && c->depth == 32) fmt = DRM_FORMAT_ARGB8888;
	else if (c->bpp == 16 && c->depth == 16) fmt = DRM_FORMAT_RGB565;
	else return -EINVAL_;
	return fb_create(fp, c->width, c->height, fmt, c->handle, c->pitch, 0, &c->fb_id);
}

int drm_ioctl_addfb2(drm_filep_t *fp, void *d)
{
	struct drm_mode_fb_cmd2 *c = d;
	if (c->flags & ~(uint32_t)DRM_MODE_FB_MODIFIERS) return -EINVAL_;
	if ((c->flags & DRM_MODE_FB_MODIFIERS) && c->modifier[0] != DRM_FORMAT_MOD_LINEAR) return -EINVAL_;
	for (int i = 1; i < 4; i++) if (c->handles[i] || c->pitches[i] || c->offsets[i]) return -EINVAL_;
	return fb_create(fp, c->width, c->height, c->pixel_format, c->handles[0], c->pitches[0], c->offsets[0], &c->fb_id);
}

static int fb_remove(drm_filep_t *fp, uint32_t id, int disable)
{
	for (drm_fb_t **pp = &K.fbs; *pp; pp = &(*pp)->next)
		if ((*pp)->id == id) {
			drm_fb_t *fb = *pp;
			if (fb->owner != fp) return -ENOENT_;
			*pp = fb->next;
			if (K.scan == fb && disable) crtc_disable(0);
			fb_put(fb);
			return 0;
		}
	return -ENOENT_;
}

int drm_ioctl_rmfb(drm_filep_t *fp, void *d) { reap_flip(); return fb_remove(fp, *(uint32_t *)d, 1); }

int drm_ioctl_closefb(drm_filep_t *fp, void *d)
{
	struct drm_mode_closefb *c = d;
	if (c->pad) return -EINVAL_;
	reap_flip();
	return fb_remove(fp, c->fb_id, 0);
}

static int getfb_common(drm_filep_t *fp, uint32_t id, drm_fb_t **out, uint32_t *handle)
{
	drm_fb_t *fb = fb_lookup(id);
	if (!fb) return -ENOENT_;
	*out = fb;
	*handle = 0;
	return drm_gem_handle_new(fp, fb->obj, handle);
}

int drm_ioctl_getfb(drm_filep_t *fp, void *d)
{
	struct drm_mode_fb_cmd *c = d;
	drm_fb_t *fb;
	int e = getfb_common(fp, c->fb_id, &fb, &c->handle);
	if (e) return e;
	c->width = fb->width; c->height = fb->height; c->pitch = fb->pitch;
	c->bpp = fb->cpp * 8;
	c->depth = fb->format == DRM_FORMAT_RGB565 ? 16 : fb->format == DRM_FORMAT_ARGB8888 || fb->format == DRM_FORMAT_ABGR8888 ? 32 : 24;
	return 0;
}

int drm_ioctl_getfb2(drm_filep_t *fp, void *d)
{
	struct drm_mode_fb_cmd2 *c = d;
	drm_fb_t *fb;
	uint32_t h;
	int e = getfb_common(fp, c->fb_id, &fb, &h);
	if (e) return e;
	memset(c->handles, 0, sizeof(c->handles));
	memset(c->pitches, 0, sizeof(c->pitches));
	memset(c->offsets, 0, sizeof(c->offsets));
	memset(c->modifier, 0, sizeof(c->modifier));
	c->width = fb->width; c->height = fb->height; c->pixel_format = fb->format;
	c->flags = DRM_MODE_FB_MODIFIERS;
	c->handles[0] = h; c->pitches[0] = fb->pitch; c->offsets[0] = fb->offset;
	c->modifier[0] = DRM_FORMAT_MOD_LINEAR;
	return 0;
}

int drm_ioctl_page_flip(drm_filep_t *fp, void *d)
{
	struct drm_mode_crtc_page_flip *f = d;
	if (f->crtc_id != ID_CRTC) return -ENOENT_;
	if (f->flags & ~(uint32_t)(DRM_MODE_PAGE_FLIP_EVENT | DRM_MODE_PAGE_FLIP_ASYNC)) return -EINVAL_;
	if (f->flags & DRM_MODE_PAGE_FLIP_ASYNC) return -EINVAL_;      /* DRM_CAP_ASYNC_PAGE_FLIP is 0 */
	reap_flip();
	if (K.flip_pending) return -EBUSY_;
	drm_fb_t *fb = fb_lookup(f->fb_id);
	if (!fb) return -ENOENT_;
	if (!K.enabled || !K.scan) return -EINVAL_;
	if (fb->format != K.scan->format) return -EINVAL_;
	if ((uint64_t)K.cx + K.w > fb->width || (uint64_t)K.cy + K.h > fb->height) return -ENOSPC_;
	drm_pending_t *ev = NULL;
	if (f->flags & DRM_MODE_PAGE_FLIP_EVENT) {
		if (drm_event_space_take(fp, sizeof(struct drm_event_vblank))) return -ENOMEM_;
		ev = kmalloc(sizeof(*ev));
		if (!ev) {
			uint64_t fl = spin_lock_irq(&drm_ev_lock);
			fp->ev_space += sizeof(struct drm_event_vblank);
			spin_unlock_irq(&drm_ev_lock, fl);
			return -ENOMEM_;
		}
		memset(ev, 0, sizeof(*ev));
		ev->file = fp;
		ev->kind = DRM_EVENT_FLIP_COMPLETE;
		ev->len = sizeof(struct drm_event_vblank);
		ev->ev.vbl.base.type = DRM_EVENT_FLIP_COMPLETE;
		ev->ev.vbl.base.length = ev->len;
		ev->ev.vbl.user_data = f->user_data;
	}
	fb->refcount++;
	return show(fb, ev, 1);
}

int drm_ioctl_dirtyfb(drm_filep_t *fp, void *d)
{
	(void)fp;
	struct drm_mode_fb_dirty_cmd *c = d;
	drm_fb_t *fb = fb_lookup(c->fb_id);
	if (!fb) return -ENOENT_;
	if (c->num_clips > DRM_MODE_FB_DIRTY_MAX_CLIPS) return -EINVAL_;
	if (fb != K.scan || !K.enabled) return 0;
	if (K.hw_scan) {                          /* the plane reads memory: push the CPU's writes out */
		drm_gem_flush(fb->obj, fb->offset, (uint64_t)fb->pitch * fb->height);
		return 0;
	}
	if (!c->num_clips) { present_all(); return 0; }
	for (uint32_t i = 0; i < c->num_clips; i++) {
		struct drm_clip_rect r;
		if (copy_from_user(&r, (void *)(uintptr_t)(c->clips_ptr + sizeof(r) * i), sizeof(r))) return -EFAULT_;
		present((int)r.x1 - (int)K.cx, (int)r.y1 - (int)K.cy, (int)r.x2 - (int)K.cx, (int)r.y2 - (int)K.cy);
	}
	return 0;
}

int drm_ioctl_getplaneresources(drm_filep_t *fp, void *d)
{
	struct drm_mode_get_plane_res *r = d;
	uint32_t ids[2] = { ID_PLANE_PRIMARY, ID_PLANE_CURSOR };
	uint32_t n = fp->universal_planes ? 2 : 0;       /* no overlays */
	int e = put_ids(r->plane_id_ptr, r->count_planes, ids, n);
	if (e) return e;
	r->count_planes = n;
	return 0;
}

static const uint32_t primary_formats[] = {
	DRM_FORMAT_XRGB8888, DRM_FORMAT_ARGB8888, DRM_FORMAT_XBGR8888, DRM_FORMAT_ABGR8888, DRM_FORMAT_RGB565,
};

int drm_ioctl_getplane(drm_filep_t *fp, void *d)
{
	(void)fp;
	struct drm_mode_get_plane *p = d;
	const uint32_t *fmts; uint32_t n;
	static const uint32_t cursor_formats[] = { DRM_FORMAT_ARGB8888 };
	if (p->plane_id == ID_PLANE_PRIMARY) {
		fmts = primary_formats; n = 5;
		p->crtc_id = K.enabled ? ID_CRTC : 0;
		p->fb_id = K.enabled && K.scan ? K.scan->id : 0;
	} else if (p->plane_id == ID_PLANE_CURSOR) {
		fmts = cursor_formats; n = 1;
		p->crtc_id = K.cur ? ID_CRTC : 0;
		p->fb_id = 0;
	} else return -ENOENT_;
	int e = put_ids(p->format_type_ptr, p->count_format_types, fmts, n);
	if (e) return e;
	p->count_format_types = n;
	p->possible_crtcs = 1;
	p->gamma_size = 0;
	return 0;
}

int drm_ioctl_setplane(drm_filep_t *fp, void *d)
{
	struct drm_mode_set_plane *s = d;
	if (s->plane_id == ID_PLANE_CURSOR) {
		if (!s->fb_id) return cursor_set(DRM_MODE_CURSOR_BO, ID_CRTC, 0, 0, 0, 0, 0, fp);
		drm_fb_t *fb = fb_lookup(s->fb_id);
		if (!fb) return -ENOENT_;
		if (fb->format != DRM_FORMAT_ARGB8888 || fb->offset || fb->width > 256 || fb->height > 256) return -EINVAL_;
		if (s->crtc_id != ID_CRTC) return -ENOENT_;
		drm_gem_put(K.cur);
		drm_gem_get(fb->obj);
		K.cur = fb->obj;
		K.cur_w = fb->width; K.cur_h = fb->height; K.cur_pitch = fb->pitch;
		return cursor_set(DRM_MODE_CURSOR_MOVE, ID_CRTC, s->crtc_x, s->crtc_y, 0, 0, 0, fp);
	}
	if (s->plane_id != ID_PLANE_PRIMARY) return -ENOENT_;
	if (!s->fb_id) { reap_flip(); crtc_disable(1); return 0; }
	if (s->crtc_id != ID_CRTC) return -ENOENT_;
	drm_fb_t *fb = fb_lookup(s->fb_id);
	if (!fb) return -ENOENT_;
	/* the primary plane covers the CRTC exactly, unscaled */
	if (s->crtc_x || s->crtc_y || s->crtc_w != K.w || s->crtc_h != K.h) return -EINVAL_;
	if ((s->src_w >> 16) != K.w || (s->src_h >> 16) != K.h || (s->src_x & 0xFFFF) || (s->src_y & 0xFFFF)) return -EINVAL_;
	uint32_t sx = s->src_x >> 16, sy = s->src_y >> 16;
	if ((uint64_t)sx + K.w > fb->width || (uint64_t)sy + K.h > fb->height) return -ENOSPC_;
	reap_flip();
	flip_settle();
	fb->refcount++;
	K.cx = sx; K.cy = sy;
	K.enabled = 1;
	int e = show(fb, NULL, 1);
	flip_settle();
	return e;
}

/* ── vblank waits and sequence events ─────────────────────────── */

static int queue_seq_event(drm_filep_t *fp, int kind, uint64_t target, uint64_t user)
{
	uint32_t len = kind == DRM_EVENT_CRTC_SEQUENCE ? sizeof(struct drm_event_crtc_sequence)
	                                               : sizeof(struct drm_event_vblank);
	if (drm_event_space_take(fp, len)) return -ENOMEM_;
	drm_pending_t *p = kmalloc(sizeof(*p));
	if (!p) {
		uint64_t fl = spin_lock_irq(&drm_ev_lock);
		fp->ev_space += len;
		spin_unlock_irq(&drm_ev_lock, fl);
		return -ENOMEM_;
	}
	memset(p, 0, sizeof(*p));
	p->file = fp; p->kind = kind; p->len = len; p->target = target;
	p->ev.base.type = (uint32_t)kind;
	p->ev.base.length = len;
	if (kind == DRM_EVENT_CRTC_SEQUENCE) p->ev.seq.user_data = user;
	else p->ev.vbl.user_data = user;
	uint64_t f = spin_lock_irq(&drm_ev_lock);
	vbl_update_locked();
	if (target <= K.vbl_seq) { stamp(p, K.vbl_seq, K.vbl_ns); drm_event_deliver(p); }
	else { p->next = K.pending; K.pending = p; }
	spin_unlock_irq(&drm_ev_lock, f);
	return 0;
}

int drm_ioctl_wait_vblank(drm_filep_t *fp, void *d)
{
	union drm_wait_vblank *w = d;
	uint32_t type = w->request.type;
	uint32_t crtc = (type & _DRM_VBLANK_HIGH_CRTC_MASK) >> _DRM_VBLANK_HIGH_CRTC_SHIFT;
	if ((type & _DRM_VBLANK_SECONDARY) || crtc) return -EINVAL_;
	if (type & (_DRM_VBLANK_SIGNAL | _DRM_VBLANK_FLIP)) return -EINVAL_;
	if (type & ~(uint32_t)(_DRM_VBLANK_TYPES_MASK | _DRM_VBLANK_EVENT | _DRM_VBLANK_NEXTONMISS)) return -EINVAL_;
	if (!K.ok) return -EINVAL_;
	uint64_t ns, cur = vbl_now(&ns);
	/* the 32-bit request names a sequence near the current one */
	uint64_t target = (type & _DRM_VBLANK_RELATIVE) ? cur + w->request.sequence
	                : (cur & ~0xFFFFFFFFULL) | w->request.sequence;
	if ((type & _DRM_VBLANK_NEXTONMISS) && (int32_t)((uint32_t)cur - (uint32_t)target) >= 0) target = cur + 1;
	if (type & _DRM_VBLANK_EVENT) {
		if (mutex_lock_killable(&drm_lock)) return -EINTR_;
		int e = queue_seq_event(fp, DRM_EVENT_VBLANK, target, w->request.signal);
		mutex_unlock(&drm_lock);
		if (e) return e;
		w->reply.type = type;
		w->reply.sequence = (uint32_t)target;
		w->reply.tval_sec = (int64_t)(ns / 1000000000ULL);
		w->reply.tval_usec = (int64_t)(ns % 1000000000ULL / 1000);
		return 0;
	}
	uint64_t give_up = drm_now_ns() + 3000000000ULL;   /* a stopped pipe must not hang the caller */
	while ((int32_t)((uint32_t)cur - (uint32_t)target) < 0) {
		if (drm_now_ns() > give_up) return -EBUSY_;
		drm_sleep_ms(1);
		if (task_has_actionable_signal()) return -EINTR_;
		cur = vbl_now(&ns);
	}
	w->reply.type = type;
	w->reply.sequence = (uint32_t)cur;
	w->reply.tval_sec = (int64_t)(ns / 1000000000ULL);
	w->reply.tval_usec = (int64_t)(ns % 1000000000ULL / 1000);
	return 0;
}

int drm_ioctl_crtc_get_sequence(drm_filep_t *fp, void *d)
{
	(void)fp;
	struct drm_crtc_get_sequence *g = d;
	if (g->crtc_id != ID_CRTC) return -ENOENT_;
	uint64_t ns;
	g->sequence = vbl_now(&ns);
	g->sequence_ns = (int64_t)ns;
	g->active = (uint32_t)K.enabled;
	return 0;
}

int drm_ioctl_crtc_queue_sequence(drm_filep_t *fp, void *d)
{
	struct drm_crtc_queue_sequence *q = d;
	if (q->crtc_id != ID_CRTC) return -ENOENT_;
	if (q->flags & ~(uint32_t)(DRM_CRTC_SEQUENCE_RELATIVE | DRM_CRTC_SEQUENCE_NEXT_ON_MISS)) return -EINVAL_;
	uint64_t cur = vbl_now(NULL);
	uint64_t target = (q->flags & DRM_CRTC_SEQUENCE_RELATIVE) ? cur + q->sequence : q->sequence;
	if ((q->flags & DRM_CRTC_SEQUENCE_NEXT_ON_MISS) && target <= cur) target = cur + 1;
	int e = queue_seq_event(fp, DRM_EVENT_CRTC_SEQUENCE, target, q->user_data);
	if (!e) q->sequence = target;
	return e;
}

/* ── a file goes away ─────────────────────────────────────────── */

void drm_kms_file_close(drm_filep_t *fp)
{
	if (!K.ok) return;
	reap_flip();
	uint64_t f = spin_lock_irq(&drm_ev_lock);
	for (drm_pending_t **pp = &K.pending; *pp; ) {
		drm_pending_t *p = *pp;
		if (p->file == fp) { *pp = p->next; kfree(p); } else pp = &p->next;
	}
	if (K.flip_ev && K.flip_ev->file == fp) { kfree(K.flip_ev); K.flip_ev = NULL; }
	spin_unlock_irq(&drm_ev_lock, f);
	for (drm_fb_t **pp = &K.fbs; *pp; ) {
		drm_fb_t *fb = *pp;
		if (fb->owner == fp) {
			*pp = fb->next;
			if (K.scan == fb) crtc_disable(0);
			fb_put(fb);
		} else pp = &fb->next;
	}
	for (drm_blob_t **pp = &K.blobs; *pp; ) {
		drm_blob_t *b = *pp;
		if (b->owner == fp) { *pp = b->next; kfree(b); } else pp = &b->next;
	}
	if (fp == drm_master && K.cur) {            /* the master's cursor leaves with it */
		drm_gem_put(K.cur);
		K.cur = NULL;
		if (K.enabled && !K.hw_scan) present(K.cur_x, K.cur_y, K.cur_x + (int)K.cur_w, K.cur_y + (int)K.cur_h);
		K.cur_w = K.cur_h = 0;
	}
}

/* ── init ─────────────────────────────────────────────────────── */

static void mode_name(char *o, uint32_t w, uint32_t h)
{
	char t[12]; int n = 0, i = 0;
	uint32_t v = w;
	do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
	while (n) o[i++] = t[--n];
	o[i++] = 'x';
	v = h;
	do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
	while (n) o[i++] = t[--n];
	o[i] = 0;
}

int drm_kms_init(void)
{
	if (!global_fb_phys || global_fb.depth != 32 || !global_fb.width || !global_fb.height) return -1;
	K.w = global_fb.width;
	K.h = global_fb.height;
	K.fbpitch = global_fb.pitch;
	K.fb_bgr = global_fb.red_shift == 0 && global_fb.blue_shift == 16;
#ifdef __x86_64__
	K.fbmem = ioremap(global_fb_phys, global_fb.fb_size, VM_READ | VM_WRITE | VM_WC);   /* VM_DEVICE would make it UC */
#else
	K.fbmem = global_fb.fb;
#endif
	K.row = kmalloc(K.w * 4);
	if (!K.fbmem || !K.row) return -1;
	K.frame_ns = 16666667;
	K.mode.hdisplay = (uint16_t)K.w;
	K.mode.vdisplay = (uint16_t)K.h;
	drm_fw_fb_t fw = { K.w, K.h, K.fbpitch, K.fbmem, global_fb.fb_size };
	K.hw = intel_display_probe(&fw, &K.mode, &K.frame_ns, &K.hwflip);
	if (!K.hw) {
		/* a plausible 60 Hz mode around the firmware's size */
		K.mode.hsync_start = (uint16_t)(K.w + 48);
		K.mode.hsync_end = (uint16_t)(K.w + 80);
		K.mode.htotal = (uint16_t)(K.w + 160);
		K.mode.vsync_start = (uint16_t)(K.h + 3);
		K.mode.vsync_end = (uint16_t)(K.h + 8);
		K.mode.vtotal = (uint16_t)(K.h + 30);
		K.conn_type = DRM_MODE_CONNECTOR_VIRTUAL;
		K.enc_type = DRM_MODE_ENCODER_VIRTUAL;
		K.sw_base = drm_now_ns() / K.frame_ns;
	} else {
		K.conn_type = K.hw->conn_type;
		K.enc_type = K.hw->enc_type;
		uint64_t since;
		K.hwcount = K.hw->vblank && K.hw->vblank(&K.hw_last, &since);
		if (!K.hw->flip) K.hwflip = 0;
	}
	if (K.mode.htotal < K.w) K.mode.htotal = (uint16_t)(K.w + 160);
	if (K.mode.vtotal < K.h) K.mode.vtotal = (uint16_t)(K.h + 30);
	K.mode.vrefresh = (uint32_t)((1000000000ULL + K.frame_ns / 2) / K.frame_ns);
	K.mode.clock = (uint32_t)((uint64_t)K.mode.htotal * K.mode.vtotal * 1000000ULL / K.frame_ns);
	K.mode.flags = DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC;
	K.mode.type = DRM_MODE_TYPE_PREFERRED | DRM_MODE_TYPE_DRIVER;
	mode_name(K.mode.name, K.w, K.h);
	K.enabled = 1;                          /* the firmware's picture is on */
	K.ok = 1;
	return 0;
}
