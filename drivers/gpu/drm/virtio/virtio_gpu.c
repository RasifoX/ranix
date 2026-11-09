/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/*
 * Ranix: the virtio-gpu display back end for KMS (2D).
 *
 * What it does, in plain words: in a virtual machine (QEMU "-device
 * virtio-gpu-pci" or "-vga virtio") the screen is a picture the host keeps.
 * The guest tells the host, through a queue of small commands, "here is a
 * picture (a resource) made of these guest pages", "copy these rectangles of
 * the pages into the picture" and "show this picture on screen 0". This file
 * turns KMS's needs into those commands:
 *
 *   - KMS copy target: a RAM buffer of the current mode (the "shadow"), as a
 *     resource. Whatever KMS copies into it is sent to the host (damage).
 *   - flip: a client's dumb buffer becomes a resource the first time it is
 *     flipped (its own pages, no copy in the guest); each flip sends the pixels
 *     (TRANSFER_TO_HOST_2D), points the screen at it (SET_SCANOUT) and asks for
 *     a repaint (RESOURCE_FLUSH). The flip has landed when the host has
 *     processed that flush; KMS completes at most one flip per 60 Hz tick.
 *   - modes: the host's preferred size (GET_DISPLAY_INFO, re-read when the host
 *     window changes) and common sizes; switching makes a new shadow.
 *   - cursor: a 64x64 resource shown by the host on the cursor queue
 *     (UPDATE_CURSOR, MOVE_CURSOR), so moving it costs no repaint.
 *
 * No interrupts: the used rings are read from the 1 ms tick and by whoever
 * waits. No 3D yet (see arastirma/virtio-gpu-3d.md); the render node has only
 * the generic dumb/PRIME ioctls. Spec: OASIS virtio 1.2, section 5.7.
 */
#include <kernel/mm/heap.h>
#include <kernel/mm/buddy.h>
#include <kernel/lib/string.h>
#include <kernel/kprintf.h>
#include <kernel/kout.h>
#include <kernel/hal.h>
#include <kernel/sync.h>
#include <asm/mmu.h>
#include <drivers/virtio/virtio_pci.h>
#include "../drm_priv.h"

#ifdef __x86_64__

/* ── protocol (virtio 1.2, 5.7.6) ─────────────────────────────── */

#define GET_DISPLAY_INFO     0x0100
#define RESOURCE_CREATE_2D   0x0101
#define RESOURCE_UNREF       0x0102
#define SET_SCANOUT          0x0103
#define RESOURCE_FLUSH       0x0104
#define TRANSFER_TO_HOST_2D  0x0105
#define ATTACH_BACKING       0x0106
#define DETACH_BACKING       0x0107
#define UPDATE_CURSOR        0x0300
#define MOVE_CURSOR          0x0301
#define RESP_OK_NODATA       0x1100
#define RESP_OK_DISPLAY_INFO 0x1101
#define RESP_ERR             0x1200      /* 0x12xx: errors */

#define FMT_B8G8R8A8         1           /* DRM ARGB8888 on little endian */
#define FMT_B8G8R8X8         2           /* DRM XRGB8888 */
#define EVENT_DISPLAY        1           /* config.events_read: display info changed */
#define VIRTIO_GPU_DEVICE    16

typedef struct { uint32_t type, flags; uint64_t fence; uint32_t ctx; uint8_t ring, pad[3]; } hdr_t;
typedef struct { uint32_t x, y, w, h; } rect_t;
typedef struct { hdr_t h; rect_t r; uint32_t scanout, res; } set_scanout_t;
typedef struct { hdr_t h; rect_t r; uint32_t res, pad; } flush_t;
typedef struct { hdr_t h; rect_t r; uint64_t offset; uint32_t res, pad; } xfer_t;
typedef struct { hdr_t h; uint32_t res, fmt, w, h2; } create_t;
typedef struct { hdr_t h; uint32_t res, pad; } unref_t;
typedef struct { hdr_t h; uint32_t res, n; } attach_t;               /* + n entries */
typedef struct { uint64_t addr; uint32_t len, pad; } entry_t;
typedef struct { hdr_t h; uint32_t scanout, x, y, pad; uint32_t res, hot_x, hot_y, pad2; } cursor_t;
typedef struct { hdr_t h; struct { rect_t r; uint32_t enabled, flags; } pm[16]; } info_t;
_Static_assert(sizeof(hdr_t) == 24 && sizeof(xfer_t) == 56 && sizeof(cursor_t) == 56 && sizeof(info_t) == 408,
               "virtio-gpu wire sizes");

/* ── state ────────────────────────────────────────────────────── */

#define NSLOT     16             /* control commands in flight */
#define SLOT_SZ   1024           /* request in the first half, response in the second */
#define NCUR      8              /* cursor commands in flight */
#define NRES      32             /* client buffers with a host resource */
#define MAX_BYTES (16u << 20)    /* largest shadow: one 16 MiB buddy block (2560x1600) */
#define WAIT_NS   1000000000ULL  /* a synchronous command that takes longer has failed */

enum { FREE, ASYNC, SYNC, SYNC_DONE };

typedef struct { drm_gem_t *o; uint32_t id, w, h; } res_t;

static struct {
	virtio_pci_t vp;
	virtq_t      ctrl, curq;
	spinlock_t   lock;           /* rings and slots; taken from the tick too */
	uint8_t     *slots;          /* NSLOT * SLOT_SZ */
	uint64_t     slots_phys;
	uint8_t      state[NSLOT];
	uint32_t     seq[NSLOT], next_seq;
	uint8_t      is_info[NSLOT]; /* async GET_DISPLAY_INFO: read the answer when it comes */
	uint8_t     *cbuf;           /* NCUR * 64 cursor commands */
	uint64_t     cbuf_phys;
	uint8_t      cbusy[NCUR];
	uint32_t     next_id;        /* resource ids */
	uint32_t     errors, last_err;

	uint32_t pref_w, pref_h;     /* the host's size for screen 0 */
	uint32_t w, h;               /* current mode */
	uint32_t shadow_id, shadow_pitch;
	uint8_t *shadow;
	uint64_t shadow_phys;
	int      shadow_order;
	uint32_t scan_id;            /* resource on screen 0, 0 = none yet */
	uint32_t flip_seq;           /* the flush that completes the last flip */
	res_t    res[NRES];
	uint32_t cur_id;             /* 64x64 cursor resource */
	uint8_t *cur_mem;
	uint64_t cur_phys;
	int      cur_on;
	uint32_t ticks;
	uint64_t flips, damages;
} V = { .lock = SPINLOCK_INIT };

static uint8_t *slot_req(int i)  { return V.slots + (uint64_t)i * SLOT_SZ; }
static uint8_t *slot_resp(int i) { return V.slots + (uint64_t)i * SLOT_SZ + SLOT_SZ / 2; }

static void read_info(const info_t *in)
{
	if (in->h.type != RESP_OK_DISPLAY_INFO || !in->pm[0].enabled) return;
	if (in->pm[0].r.w && in->pm[0].r.h && in->pm[0].r.w <= 8192 && in->pm[0].r.h <= 8192) {
		V.pref_w = in->pm[0].r.w;
		V.pref_h = in->pm[0].r.h;
	}
}

/* Take what the device finished off both rings. Any context. */
static void reap(void)
{
	uint64_t f = spin_lock_irq(&V.lock);
	uint16_t head;
	while (virtq_pop(&V.ctrl, &head, NULL)) {
		int i = head / 2;
		if (i >= NSLOT || V.state[i] == FREE) continue;
		uint32_t t = ((hdr_t *)slot_resp(i))->type;
		if (t >= RESP_ERR) { V.errors++; V.last_err = t; }
		if (V.state[i] == SYNC) { V.state[i] = SYNC_DONE; continue; }
		if (V.is_info[i]) { read_info((info_t *)slot_resp(i)); V.is_info[i] = 0; }
		V.state[i] = FREE;
	}
	while (virtq_pop(&V.curq, &head, NULL))
		if (head < NCUR) V.cbusy[head] = 0;
	spin_unlock_irq(&V.lock, f);
}

/* Queue a control command. req_phys: a request outside the slot (one with
 * backing entries), else req/len is copied into the slot. Returns the slot, or
 * -1 when every slot stays busy (only `wait` = 0 gives up at once). */
static int submit(const void *req, uint32_t len, uint64_t req_phys, uint32_t resp_len, int st, int wait, int info, uint32_t *seq)
{
	uint64_t give_up = drm_now_ns() + WAIT_NS;
	for (;;) {
		uint64_t f = spin_lock_irq(&V.lock);
		for (int i = 0; i < NSLOT; i++) {
			if (V.state[i] != FREE) continue;
			if (!req_phys) { memcpy(slot_req(i), req, len); req_phys = V.slots_phys + (uint64_t)i * SLOT_SZ; }
			memset(slot_resp(i), 0, resp_len);
			virtq_desc_t *d = &V.ctrl.desc[2 * i];
			d[0] = (virtq_desc_t){ req_phys, len, VIRTQ_DESC_F_NEXT, (uint16_t)(2 * i + 1) };
			d[1] = (virtq_desc_t){ V.slots_phys + (uint64_t)i * SLOT_SZ + SLOT_SZ / 2, resp_len, VIRTQ_DESC_F_WRITE, 0 };
			V.state[i] = (uint8_t)st;
			V.is_info[i] = (uint8_t)info;
			V.seq[i] = ++V.next_seq;
			if (seq) *seq = V.seq[i];              /* slot i may be reaped right after */
			virtq_kick(&V.ctrl, (uint16_t)(2 * i));
			spin_unlock_irq(&V.lock, f);
			return i;
		}
		spin_unlock_irq(&V.lock, f);
		if (!wait || drm_now_ns() > give_up) return -1;
		reap();
		cpu_relax();
	}
}

/* fire and forget: the answer is checked (and counted) when it is reaped */
static int cmd_async(const void *req, uint32_t len)
{
	uint32_t seq;
	int i = submit(req, len, 0, sizeof(hdr_t), ASYNC, 1, 0, &seq);
	if (i < 0) { V.errors++; return -1; }
	return (int)seq;
}

/* wait for the answer; 0 when the host said OK. resp may be NULL. */
static int cmd_sync_phys(const void *req, uint32_t len, uint64_t req_phys, void *resp, uint32_t resp_len)
{
	if (resp_len < sizeof(hdr_t)) resp_len = sizeof(hdr_t);
	int i = submit(req, len, req_phys, resp_len, SYNC, 1, 0, NULL);
	if (i < 0) { V.errors++; return -1; }
	uint64_t give_up = drm_now_ns() + WAIT_NS;
	while (V.state[i] != SYNC_DONE) {
		reap();
		if (V.state[i] == SYNC_DONE) break;
		if (drm_now_ns() > give_up) {
			/* the slot stays taken: the host may still write its answer there */
			kprintf(KERN_ERR "virtio_gpu: command %x got no answer\n", ((hdr_t *)req)->type);
			V.errors++;
			return -1;
		}
		cpu_relax();
	}
	uint32_t t = ((hdr_t *)slot_resp(i))->type;
	if (resp) memcpy(resp, slot_resp(i), resp_len);
	uint64_t f = spin_lock_irq(&V.lock);
	V.state[i] = FREE;
	spin_unlock_irq(&V.lock, f);
	return t >= RESP_ERR ? -1 : 0;
}

static int cmd_sync(const void *req, uint32_t len) { return cmd_sync_phys(req, len, 0, NULL, 0); }

/* a cursor command; moves may be dropped when the queue is full, the rest wait */
static void cursor_cmd(const cursor_t *c, int must)
{
	uint64_t give_up = drm_now_ns() + WAIT_NS;
	for (;;) {
		uint64_t f = spin_lock_irq(&V.lock);
		for (int i = 0; i < NCUR; i++) {
			if (V.cbusy[i]) continue;
			memcpy(V.cbuf + 64 * i, c, sizeof(*c));
			V.curq.desc[i] = (virtq_desc_t){ V.cbuf_phys + 64 * (uint64_t)i, sizeof(*c), 0, 0 };
			V.cbusy[i] = 1;
			virtq_kick(&V.curq, (uint16_t)i);
			spin_unlock_irq(&V.lock, f);
			return;
		}
		spin_unlock_irq(&V.lock, f);
		if (!must || drm_now_ns() > give_up) return;
		reap();
		cpu_relax();
	}
}

/* ── resources ────────────────────────────────────────────────── */

static hdr_t H(uint32_t type) { return (hdr_t){ .type = type }; }

static int create(uint32_t id, uint32_t fmt, uint32_t w, uint32_t h)
{
	create_t c = { H(RESOURCE_CREATE_2D), id, fmt, w, h };
	return cmd_sync(&c, sizeof(c));
}

static void destroy(uint32_t id)
{
	unref_t d = { H(DETACH_BACKING), id, 0 };
	cmd_sync(&d, sizeof(d));                    /* the host lets go of the pages first */
	unref_t u = { H(RESOURCE_UNREF), id, 0 };
	cmd_sync(&u, sizeof(u));
}

/* Give the host the guest pages behind a resource: contiguous runs as one
 * entry each. pages == NULL: one run at phys. */
static int attach(uint32_t id, const uint64_t *pages, uint32_t npages, uint64_t phys, uint64_t bytes)
{
	uint32_t n = 0, cap = pages ? npages : 1;
	uint64_t need = sizeof(attach_t) + (uint64_t)cap * sizeof(entry_t);
	int ord = 0;
	while ((4096ULL << ord) < need) ord++;
	void *bp = buddy_alloc(ord);
	if (!bp) return -1;
	attach_t *a = (attach_t *)PHYS_TO_VIRT(bp);
	entry_t *e = (entry_t *)(a + 1);
	if (!pages) e[n++] = (entry_t){ phys, (uint32_t)bytes, 0 };
	else
		for (uint32_t i = 0; i < npages && bytes; i++) {
			uint32_t l = bytes < 4096 ? (uint32_t)bytes : 4096;
			if (n && e[n - 1].addr + e[n - 1].len == pages[i] && e[n - 1].len < (1u << 30)) e[n - 1].len += l;
			else e[n++] = (entry_t){ pages[i], l, 0 };
			bytes -= l;
		}
	*a = (attach_t){ H(ATTACH_BACKING), id, n };
	int r = cmd_sync_phys(a, (uint32_t)(sizeof(*a) + n * sizeof(entry_t)), (uint64_t)bp, NULL, 0);
	buddy_free((uint64_t)bp, ord);
	return r;
}

static int scanout(uint32_t id, uint32_t w, uint32_t h, int sync)
{
	set_scanout_t s = { H(SET_SCANOUT), { 0, 0, w, h }, 0, id };
	V.scan_id = id;
	return sync ? cmd_sync(&s, sizeof(s)) : cmd_async(&s, sizeof(s));
}

/* send rectangle r of resource id (rows `pitch` bytes apart) and repaint it;
 * returns the flush's sequence number */
static int push(uint32_t id, uint32_t pitch, rect_t r)
{
	xfer_t t = { H(TRANSFER_TO_HOST_2D), r, (uint64_t)r.y * pitch + (uint64_t)r.x * 4, id, 0 };
	cmd_async(&t, sizeof(t));
	flush_t fl = { H(RESOURCE_FLUSH), r, id, 0 };
	return cmd_async(&fl, sizeof(fl));
}

/* the shadow: KMS's copy target for w x h, as a resource */
static int shadow_new(uint32_t w, uint32_t h)
{
	uint64_t bytes = (uint64_t)w * h * 4;
	if (!w || !h || bytes > MAX_BYTES) return -1;
	int ord = 0;
	while ((4096ULL << ord) < bytes) ord++;
	void *p = buddy_alloc(ord);
	if (!p) return -1;
	uint32_t id = ++V.next_id;
	if (create(id, FMT_B8G8R8X8, w, h) || attach(id, NULL, 0, (uint64_t)p, bytes)) {
		unref_t u = { H(RESOURCE_UNREF), id, 0 };
		cmd_sync(&u, sizeof(u));
		buddy_free((uint64_t)p, ord);
		return -1;
	}
	V.shadow_id = id;
	V.shadow_phys = (uint64_t)p;
	V.shadow_order = ord;
	V.shadow = (uint8_t *)PHYS_TO_VIRT(p);
	V.shadow_pitch = w * 4;
	memset(V.shadow, 0, bytes);
	V.w = w; V.h = h;
	return 0;
}

static res_t *res_find(drm_gem_t *o)
{
	for (int i = 0; i < NRES; i++) if (V.res[i].o == o) return &V.res[i];
	return NULL;
}

static void res_drop(res_t *r)
{
	if (V.scan_id == r->id) scanout(V.shadow_id, V.w, V.h, 1);
	destroy(r->id);
	memset(r, 0, sizeof(*r));
}

/* the resource showing object o as w x h, made on first use */
static res_t *res_for(drm_gem_t *o, uint32_t w, uint32_t h)
{
	res_t *r = res_find(o);
	if (r && r->w == w && r->h == h) return r;
	if (r) res_drop(r);                          /* other size or stride: make it again */
	r = res_find(NULL);
	if (!r) {                                    /* full: drop one that is not on screen */
		for (int i = 0; i < NRES && !r; i++) if (V.res[i].id != V.scan_id) r = &V.res[i];
		if (!r) return NULL;
		res_drop(r);
	}
	uint32_t id = ++V.next_id;
	if (create(id, FMT_B8G8R8X8, w, h)) return NULL;
	if (attach(id, o->pages, o->npages, 0, (uint64_t)w * 4 * h)) {
		unref_t u = { H(RESOURCE_UNREF), id, 0 };
		cmd_sync(&u, sizeof(u));
		return NULL;
	}
	*r = (res_t){ o, id, w, h };
	return r;
}

/* ── drm_display_t ────────────────────────────────────────────── */

static int flip(drm_gem_t *o, uint32_t pitch)
{
	if (pitch & 3 || (uint64_t)pitch * V.h > o->size) return -1;
	res_t *r = res_for(o, pitch / 4, V.h);
	if (!r) return -1;
	rect_t all = { 0, 0, V.w, V.h };
	xfer_t t = { H(TRANSFER_TO_HOST_2D), all, 0, r->id, 0 };
	cmd_async(&t, sizeof(t));
	if (V.scan_id != r->id) scanout(r->id, V.w, V.h, 0);
	flush_t fl = { H(RESOURCE_FLUSH), all, r->id, 0 };
	V.flip_seq = (uint32_t)cmd_async(&fl, sizeof(fl));
	V.flips++;
	return 0;
}

static void flip_firmware(void)
{
	scanout(V.shadow_id, V.w, V.h, 0);
	flush_t fl = { H(RESOURCE_FLUSH), { 0, 0, V.w, V.h }, V.shadow_id, 0 };
	V.flip_seq = (uint32_t)cmd_async(&fl, sizeof(fl));
}

/* the flush that ended the last flip has been processed by the host */
static int flip_landed(void)
{
	reap();
	for (int i = 0; i < NSLOT; i++) if (V.state[i] != FREE && V.seq[i] == V.flip_seq) return 0;
	return 1;
}

static void release(drm_gem_t *o)
{
	res_t *r = res_find(o);
	if (r) res_drop(r);
}

static void damage(drm_gem_t *o, int x1, int y1, int x2, int y2)
{
	uint32_t id, pitch, w, h;
	if (!o) { id = V.shadow_id; pitch = V.shadow_pitch; w = V.w; h = V.h; }
	else {
		res_t *r = res_find(o);
		if (!r) return;                          /* never flipped: not on screen */
		id = r->id; pitch = r->w * 4; w = r->w; h = r->h;
	}
	if (x1 < 0) x1 = 0;
	if (y1 < 0) y1 = 0;
	if (x2 > (int)w) x2 = (int)w;
	if (y2 > (int)h) y2 = (int)h;
	if (x1 >= x2 || y1 >= y2) return;
	if (!V.scan_id && !o) scanout(V.shadow_id, V.w, V.h, 0);   /* the first picture */
	push(id, pitch, (rect_t){ (uint32_t)x1, (uint32_t)y1, (uint32_t)(x2 - x1), (uint32_t)(y2 - y1) });
	V.damages++;
}

static int set_mode(uint32_t w, uint32_t h, drm_fw_fb_t *fb)
{
	uint32_t old_id = V.shadow_id, ow = V.w, oh = V.h;
	uint64_t old_phys = V.shadow_phys;
	int old_ord = V.shadow_order;
	if (shadow_new(w, h)) return -1;
	if (scanout(V.shadow_id, w, h, 1)) {         /* the host refused: keep the old mode */
		destroy(V.shadow_id);
		buddy_free(V.shadow_phys, V.shadow_order);
		V.shadow_id = old_id; V.shadow_phys = old_phys; V.shadow_order = old_ord;
		V.shadow = (uint8_t *)PHYS_TO_VIRT(old_phys); V.shadow_pitch = ow * 4; V.w = ow; V.h = oh;
		return -1;
	}
	push(V.shadow_id, V.shadow_pitch, (rect_t){ 0, 0, w, h });
	destroy(old_id);
	buddy_free(old_phys, old_ord);
	*fb = (drm_fw_fb_t){ w, h, V.shadow_pitch, V.shadow, (uint64_t)w * h * 4 };
	return 0;
}

static const uint16_t std_modes[][2] = {
	{ 2560, 1600 }, { 2560, 1440 }, { 1920, 1200 }, { 1920, 1080 }, { 1680, 1050 }, { 1600, 900 },
	{ 1440, 900 }, { 1366, 768 }, { 1280, 1024 }, { 1280, 800 }, { 1280, 720 }, { 1024, 768 },
	{ 800, 600 }, { 640, 480 },
};

static int fits(uint32_t w, uint32_t h) { return w && h && (uint64_t)w * h * 4 <= MAX_BYTES; }

static int add_mode(uint32_t *wh, int n, uint32_t w, uint32_t h)
{
	for (int i = 0; i < n; i++) if (wh[2 * i] == w && wh[2 * i + 1] == h) return n;
	wh[2 * n] = w; wh[2 * n + 1] = h;
	return n + 1;
}

/* preferred (the host's size) first, then the current one, then common sizes */
static int modes(uint32_t *wh, int max)
{
	int n = 0;
	if (fits(V.pref_w, V.pref_h)) n = add_mode(wh, n, V.pref_w, V.pref_h);
	if (n < max) n = add_mode(wh, n, V.w, V.h);
	for (unsigned i = 0; i < sizeof(std_modes) / sizeof(std_modes[0]) && n < max; i++)
		n = add_mode(wh, n, std_modes[i][0], std_modes[i][1]);
	return n;
}

static int cursor(drm_gem_t *o, uint32_t w, uint32_t h, uint32_t pitch, int32_t x, int32_t y)
{
	cursor_t c = { H(UPDATE_CURSOR), 0, (uint32_t)x, (uint32_t)y, 0, 0, 0, 0, 0 };
	if (!o || w > 64 || h > 64 || !V.cur_id) {
		if (V.cur_on) { cursor_cmd(&c, 1); V.cur_on = 0; }   /* resource 0: hidden */
		return o ? -1 : 0;                       /* too big: KMS draws it */
	}
	memset(V.cur_mem, 0, 64 * 64 * 4);
	for (uint32_t row = 0; row < h; row++)
		for (uint32_t col = 0; col < w; col++) {
			uint32_t *s = drm_gem_vaddr(o, (uint64_t)row * pitch + (uint64_t)col * 4, NULL);
			((uint32_t *)V.cur_mem)[row * 64 + col] = s ? *s : 0;
		}
	xfer_t t = { H(TRANSFER_TO_HOST_2D), { 0, 0, 64, 64 }, 0, V.cur_id, 0 };
	if (cmd_sync(&t, sizeof(t))) return -1;     /* the image is in before the cursor queue uses it */
	c.res = V.cur_id;
	cursor_cmd(&c, 1);
	V.cur_on = 1;
	return 0;
}

static void cursor_move(int32_t x, int32_t y)
{
	if (!V.cur_on) return;
	cursor_t c = { H(MOVE_CURSOR), 0, (uint32_t)x, (uint32_t)y, 0, 0, 0, 0, 0 };
	cursor_cmd(&c, 0);
}

/* 1 ms tick, interrupt context: reap; every 100 ms see if the host's size changed */
static void tick(void)
{
	reap();
	if (++V.ticks % 100) return;
	volatile uint32_t *cfg = (volatile uint32_t *)V.vp.device;
	if (!(cfg[0] & EVENT_DISPLAY)) return;
	hdr_t q = H(GET_DISPLAY_INFO);               /* the answer is read in reap() */
	if (submit(&q, sizeof(q), 0, sizeof(info_t), ASYNC, 0, 1, NULL) >= 0)
		cfg[1] = EVENT_DISPLAY;                  /* events_clear; a full queue tries again next time */
}

static int debug(char *o, int max)
{
	kout_t k;
	kout_init(&k, o, (size_t)max);
	kout_s(&k, "virtio-gpu mode="); kout_u(&k, V.w); kout_c(&k, 'x'); kout_u(&k, V.h);
	kout_s(&k, " pref="); kout_u(&k, V.pref_w); kout_c(&k, 'x'); kout_u(&k, V.pref_h);
	kout_s(&k, " screen="); kout_u(&k, V.scan_id);
	kout_s(&k, V.scan_id == V.shadow_id ? "(shadow)" : V.scan_id ? "(client)" : "(none)");
	int n = 0;
	for (int i = 0; i < NRES; i++) n += V.res[i].o != NULL;
	kout_s(&k, " res="); kout_u(&k, (uint64_t)n);
	kout_s(&k, " flips="); kout_u(&k, V.flips);
	kout_s(&k, " damages="); kout_u(&k, V.damages);
	kout_s(&k, " cursor="); kout_s(&k, V.cur_on ? "hw" : "off");
	kout_s(&k, " err="); kout_u(&k, V.errors);
	return (int)k.len;
}

static const drm_display_t virtio_display = {
	.name = "virtio_gpu", .desc = "virtio GPU (Ranix, 2D)", .sysfs_driver = "pci/drivers/virtio-pci",
	.version_minor = 1,
	.conn_type = DRM_MODE_CONNECTOR_VIRTUAL, .enc_type = DRM_MODE_ENCODER_VIRTUAL,
	.flip = flip, .flip_firmware = flip_firmware, .flip_landed = flip_landed,
	.release = release, .tick = tick, .debug = debug,
	.modes = modes, .set_mode = set_mode, .damage = damage,
	.cursor = cursor, .cursor_move = cursor_move,
};

/* ── probe ────────────────────────────────────────────────────── */

static int queues(void)
{
	void *p = buddy_alloc(2);                    /* 16 KiB of command slots */
	void *c = buddy_alloc(0);                    /* cursor commands */
	if (!p || !c) return -1;
	V.slots_phys = (uint64_t)p;
	V.slots = (uint8_t *)PHYS_TO_VIRT(p);
	V.cbuf_phys = (uint64_t)c;
	V.cbuf = (uint8_t *)PHYS_TO_VIRT(c);
	memset(V.slots, 0, NSLOT * SLOT_SZ);
	memset(V.cbuf, 0, 4096);
	if (virtio_pci_queue(&V.vp, 0, 2 * NSLOT, &V.ctrl) || virtio_pci_queue(&V.vp, 1, NCUR, &V.curq)) return -1;
	return V.ctrl.size == 2 * NSLOT && V.curq.size == NCUR ? 0 : -1;
}

const drm_display_t *virtio_gpu_probe(drm_fw_fb_t *fb, struct drm_mode_modeinfo *mode,
                                      uint64_t *frame_ns, int *can_flip)
{
	(void)frame_ns;                              /* no beam: KMS keeps its 60 Hz clock */
	if (virtio_pci_find(VIRTIO_GPU_DEVICE, &V.vp)) return NULL;
	if (virtio_pci_start(&V.vp, 0) || queues()) {
		kprintf(KERN_ERR "virtio_gpu: device did not start\n");
		virtio_pci_fail(&V.vp);
		return NULL;
	}
	virtio_pci_ready(&V.vp);
	info_t in;
	hdr_t q = H(GET_DISPLAY_INFO);
	if (cmd_sync_phys(&q, sizeof(q), 0, &in, sizeof(in)) == 0) read_info(&in);
	if (!fits(V.pref_w, V.pref_h)) { V.pref_w = 1024; V.pref_h = 768; }
	if (shadow_new(V.pref_w, V.pref_h)) {
		kprintf(KERN_ERR "virtio_gpu: no memory for a %ux%u screen\n", V.pref_w, V.pref_h);
		virtio_pci_fail(&V.vp);
		return NULL;
	}
	/* the cursor resource: 64x64, what the host's cursor takes */
	void *cp = buddy_alloc(2);
	if (cp) {
		V.cur_phys = (uint64_t)cp;
		V.cur_mem = (uint8_t *)PHYS_TO_VIRT(cp);
		memset(V.cur_mem, 0, 64 * 64 * 4);
		V.cur_id = ++V.next_id;
		if (create(V.cur_id, FMT_B8G8R8A8, 64, 64) || attach(V.cur_id, NULL, 0, V.cur_phys, 64 * 64 * 4)) V.cur_id = 0;
	}
	/* screen 0 stays as it is (with -vga virtio: the firmware console) until
	 * KMS first puts something up */
	drm_pci_found = 1;
	drm_pci_bus = V.vp.bus; drm_pci_dev = V.vp.dev; drm_pci_fn = V.vp.fn;
	*fb = (drm_fw_fb_t){ V.w, V.h, V.shadow_pitch, V.shadow, (uint64_t)V.w * V.h * 4 };
	mode->hdisplay = (uint16_t)V.w;
	mode->vdisplay = (uint16_t)V.h;
	*can_flip = 1;
	kprintf(KERN_INFO "virtio_gpu: %02x:%02x.%u, screen %ux%u, cursor %s\n",
	        V.vp.bus, V.vp.dev, V.vp.fn, V.w, V.h, V.cur_id ? "on the cursor queue" : "drawn by KMS");
	return &virtio_display;
}

#else

const drm_display_t *virtio_gpu_probe(drm_fw_fb_t *fb, struct drm_mode_modeinfo *mode,
                                      uint64_t *frame_ns, int *can_flip)
{
	(void)fb; (void)mode; (void)frame_ns; (void)can_flip;
	return NULL;
}

#endif
