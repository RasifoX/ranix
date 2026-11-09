// SPDX-License-Identifier: MIT
/*
 * Ranix: the i915 GEM uAPI over the stage-0 DRM core and the RCS engine
 * (ranix_gt.c). GETPARAM follows Linux i915_getparam.c (MIT, Copyright Intel
 * Corporation): the same parameter set and the same "always 1" group; the values
 * are Kaby Lake GT2's and what Ranix implements. Everything else here is Ranix's
 * own: one 48-bit PPGTT per open file, contexts with an engine map, softpin and
 * relocations, a request per execbuffer, waits by seqno.
 *
 * Not yet: other engines (BCS/VCS/VECS), VM_CREATE, userptr, real asynchronous
 * fences (an execbuffer that signals a syncobj or returns a fence waits for its
 * request first, so every fence it hands out is already signaled), tiling.
 */
#include <kernel/vfs.h>
#include <kernel/mm/heap.h>
#include <kernel/mm/buddy.h>
#include <kernel/mm/pmm.h>
#include <kernel/lib/string.h>
#include <kernel/proc.h>
#include <kernel/hal.h>
#include <kernel/pci.h>
#include <kernel/tick.h>
#include <kernel/kprintf.h>
#include "../drm_priv.h"
#include "intel_display.h"
#include "ranix_i915_uapi.h"
#include "ranix_gt.h"
#include "ranix_vm.h"

#define VA_48B   (1ULL << 48)
#define VA_LOW   0x100000ULL               /* first address handed out below 4 GiB */
#define VA_HIGH  (1ULL << 32)
#define BOUND    1ULL                      /* low bit of va[]: bound */
#define MAX_EXT  512                       /* user extension chains: i915 stops loops too */

typedef struct uctx {
	uint32_t id;
	/* One hardware context per engine-map slot, as i915 gives each slot its own
	 * intel_context: Mesa puts its render and compute batches on two slots of one
	 * context, each relying on the state it left behind. gt[0] also serves the
	 * legacy ring selection; the others are made on first use. */
	gt_ctx_t *gt[8];
	int      nengines;                     /* 0: legacy ring selection */
	struct i915_engine_class_instance eng[8];
	int32_t  priority;
	uint8_t  bannable, recoverable, persistence, no_capture;   /* set and read back, not acted on yet */
	struct uctx *next;
} uctx_t;

typedef struct {
	ppgtt_t      vm;
	uctx_t   *ctxs;
	uint32_t  next_ctx;
	uint64_t *va;                          /* per handle: address | BOUND */
	uint32_t  nva;
	uint64_t  lo_next, hi_next;
} ifile_t;

static uint16_t pci_devid, pci_rev;

static ifile_t *ifile(drm_filep_t *fp)
{
	if (fp->driver_priv) return fp->driver_priv;
	ifile_t *f = kmalloc(sizeof(*f));
	if (!f) return NULL;
	memset(f, 0, sizeof(*f));
	if (ppgtt_init(&f->vm)) { kfree(f); return NULL; }
	f->lo_next = VA_LOW;
	f->hi_next = VA_HIGH;
	f->next_ctx = 1;
	fp->driver_priv = f;
	return f;
}

static uint64_t *va_slot(ifile_t *f, uint32_t h)
{
	if (h > f->nva) {
		uint32_t n = f->nva ? f->nva : 64;
		while (n < h) n *= 2;
		uint64_t *nv = kmalloc(n * sizeof(uint64_t));
		if (!nv) return NULL;
		memset(nv, 0, n * sizeof(uint64_t));
		if (f->va) { memcpy(nv, f->va, f->nva * sizeof(uint64_t)); kfree(f->va); }
		f->va = nv;
		f->nva = n;
	}
	return &f->va[h - 1];
}

static int obj_wait(drm_gem_t *o, int64_t timeout_ns)
{
	return gt_wait_seqno(o->gpu_seqno, timeout_ns);
}

/* ── objects ── */

static int ioc_gem_create(drm_filep_t *fp, void *d)
{
	struct drm_i915_gem_create *c = d;
	uint64_t sz = (c->size + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
	if (!c->size || sz < c->size) return -EINVAL_;             /* zero, or it overflows rounding up */
	if (sz > buddy_total_pages() * PAGE_SIZE) return -E2BIG_;   /* more than the machine has */
	drm_gem_t *o = drm_gem_alloc(sz);
	if (!o) return -ENOMEM_;
	int e = drm_gem_handle_new(fp, o, &c->handle);
	c->size = o->size;
	drm_gem_put(o);
	return e;
}

static int ioc_gem_create_ext(drm_filep_t *fp, void *d)
{
	struct drm_i915_gem_create_ext *c = d;
	if (c->flags & ~1u) return -EINVAL_;                /* NEEDS_CPU_ACCESS: all memory is */
	int hops = 0;
	for (uint64_t ext = c->extensions; ext; ) {
		struct drm_i915_gem_create_ext_memory_regions mr;
		if (++hops > MAX_EXT) return -E2BIG_;             /* a chain that loops back on itself */
		if (kloop_should_stop()) return -EINTR_;
		if (copy_from_user(&mr, (void *)(uintptr_t)ext, sizeof(mr))) return -EFAULT_;
		if (mr.base.name != I915_GEM_CREATE_EXT_MEMORY_REGIONS) return -EINVAL_;
		if (hops > 1) return -EINVAL_;                  /* each extension at most once, as i915 */
		if (mr.base.flags || mr.pad || mr.base.rsvd[0] || mr.base.rsvd[1] || mr.base.rsvd[2] || mr.base.rsvd[3])
			return -EINVAL_;
		if (!mr.num_regions || mr.num_regions > 1) return -EINVAL_;
		struct drm_i915_gem_memory_class_instance r;
		if (copy_from_user(&r, (void *)(uintptr_t)mr.regions, sizeof(r))) return -EFAULT_;
		if (r.memory_class != 0 || r.memory_instance != 0) return -EINVAL_;   /* system memory only */
		ext = mr.base.next_extension;
	}
	struct drm_i915_gem_create cc = { .size = c->size };
	int e = ioc_gem_create(fp, &cc);
	c->handle = cc.handle;
	c->size = cc.size;
	return e;
}

static int rw_obj(drm_filep_t *fp, uint32_t h, uint64_t off, uint64_t size, uint64_t uptr, int write)
{
	drm_gem_t *o = drm_gem_lookup(fp, h);
	if (!o) return -ENOENT_;
	if (off > o->size || size > o->size - off) return -EINVAL_;
	while (size) {
		if (kloop_should_stop()) return -EINTR_;          /* up to the whole object, a page at a time */
		uint64_t av;
		void *p = drm_gem_vaddr(o, off, &av);
		uint64_t n = av < size ? av : size;
		long r = write ? copy_from_user(p, (void *)(uintptr_t)uptr, n) : copy_to_user((void *)(uintptr_t)uptr, p, n);
		if (r) return -EFAULT_;
		off += n; uptr += n; size -= n;
	}
	return 0;
}

static int wait_handle(drm_filep_t *fp, uint32_t h, int64_t timeout, drm_gem_t **keep);

/* like i915: pread/pwrite wait for the GPU first (they move the object to the CPU domain) */
static int rw_waited(drm_filep_t *fp, uint32_t h, uint64_t off, uint64_t size, uint64_t uptr, int write)
{
	int e = wait_handle(fp, h, -1, NULL);
	if (e && e != -EIO_) return e;
	if (mutex_lock_killable(&drm_lock)) return -EINTR_;
	e = rw_obj(fp, h, off, size, uptr, write);
	mutex_unlock(&drm_lock);
	return e;
}

static int ioc_pread(drm_filep_t *fp, void *d)
{
	struct drm_i915_gem_pread *a = d;
	return rw_waited(fp, a->handle, a->offset, a->size, a->data_ptr, 0);
}

static int ioc_pwrite(drm_filep_t *fp, void *d)
{
	struct drm_i915_gem_pwrite *a = d;
	return rw_waited(fp, a->handle, a->offset, a->size, a->data_ptr, 1);
}

static int ioc_mmap_legacy(drm_filep_t *fp, void *d) { (void)fp; (void)d; return -EOPNOTSUPP_; }

static int ioc_mmap_offset(drm_filep_t *fp, void *d)
{
	struct drm_i915_gem_mmap_offset *m = d;
	if (m->extensions) return -EINVAL_;
	if (m->flags > 4) return -EINVAL_;
	if (m->flags == I915_MMAP_OFFSET_FIXED) return -ENODEV_;   /* integrated: WB, WC, UC, GTT */
	drm_gem_t *o = drm_gem_lookup(fp, m->handle);
	if (!o) return -ENOENT_;
	m->offset = drm_gem_mmap_offset(o);        /* cached RAM; LLC keeps it coherent with the GPU */
	return 0;
}

/* lookup under drm_lock, wait without it */
static int wait_handle(drm_filep_t *fp, uint32_t h, int64_t timeout, drm_gem_t **keep)
{
	if (mutex_lock_killable(&drm_lock)) return -EINTR_;
	drm_gem_t *o = drm_gem_lookup(fp, h);
	if (o) drm_gem_get(o);
	mutex_unlock(&drm_lock);
	if (!o) return -ENOENT_;
	int e = obj_wait(o, timeout);
	if (keep) *keep = o;
	else { mutex_lock(&drm_lock); drm_gem_put(o); mutex_unlock(&drm_lock); }
	return e;
}

static int ioc_set_domain(drm_filep_t *fp, void *d)
{
	struct drm_i915_gem_set_domain *s = d;
	if (s->write_domain && s->write_domain != s->read_domains && s->read_domains) return -EINVAL_;
	int e = wait_handle(fp, s->handle, -1, NULL);
	return e == -EIO_ ? 0 : e;
}

static int ioc_sw_finish(drm_filep_t *fp, void *d)
{
	struct drm_i915_gem_sw_finish *s = d;
	return drm_gem_lookup(fp, s->handle) ? 0 : -ENOENT_;
}

static int ioc_busy(drm_filep_t *fp, void *d)
{
	struct drm_i915_gem_busy *b = d;
	drm_gem_t *o = drm_gem_lookup(fp, b->handle);
	if (!o) return -ENOENT_;
	b->busy = gt_seqno_busy(o->gpu_seqno) ? (1u << 16) | 1u : 0;  /* render reads | render (class 0) + 1 writes */
	return 0;
}

static int ioc_wait(drm_filep_t *fp, void *d)
{
	struct drm_i915_gem_wait *w = d;
	if (w->flags) return -EINVAL_;
	uint64_t t0 = drm_now_ns();
	int e = wait_handle(fp, w->bo_handle, w->timeout_ns, NULL);
	if (w->timeout_ns > 0) {
		int64_t left = w->timeout_ns - (int64_t)(drm_now_ns() - t0);
		w->timeout_ns = left > 0 ? left : 0;
	}
	return e;
}

static int ioc_throttle(drm_filep_t *fp, void *d) { (void)fp; (void)d; return 0; }

static int ioc_aperture(drm_filep_t *fp, void *d)
{
	(void)fp;
	struct drm_i915_gem_get_aperture *a = d;
	a->aper_size = a->aper_available_size = 256ULL << 20;
	return 0;
}

static int ioc_set_tiling(drm_filep_t *fp, void *d)
{
	struct drm_i915_gem_set_tiling *t = d;
	if (!drm_gem_lookup(fp, t->handle)) return -ENOENT_;
	if (t->tiling_mode != 0) return -EINVAL_;           /* fences and detiling: not yet */
	t->stride = 0; t->swizzle_mode = 0;
	return 0;
}

static int ioc_get_tiling(drm_filep_t *fp, void *d)
{
	struct drm_i915_gem_get_tiling *t = d;
	if (!drm_gem_lookup(fp, t->handle)) return -ENOENT_;
	t->tiling_mode = t->swizzle_mode = t->phys_swizzle_mode = 0;
	return 0;
}

static int ioc_madvise(drm_filep_t *fp, void *d)
{
	struct drm_i915_gem_madvise *m = d;
	if (m->madv > 1) return -EINVAL_;
	if (!drm_gem_lookup(fp, m->handle)) return -ENOENT_;
	m->retained = 1;                                   /* nothing is purged */
	return 0;
}

static int ioc_set_caching(drm_filep_t *fp, void *d)
{
	struct drm_i915_gem_caching *c = d;
	drm_gem_t *o = drm_gem_lookup(fp, c->handle);
	if (!o) return -ENOENT_;
	if (c->caching > 2) return -EINVAL_;
	o->caching = c->caching;
	return 0;
}

static int ioc_get_caching(drm_filep_t *fp, void *d)
{
	struct drm_i915_gem_caching *c = d;
	drm_gem_t *o = drm_gem_lookup(fp, c->handle);
	if (!o) return -ENOENT_;
	c->caching = o->caching ? o->caching : 1;         /* LLC: cached by default */
	return 0;
}

/* ── GETPARAM (as i915_getparam.c) ── */

static int ioc_getparam(drm_filep_t *fp, void *d)
{
	(void)fp;
	struct drm_i915_getparam *g = d;
	int value;
	switch (g->param) {
	case 1: case 2: case 3: case 14:                   /* old UMS/DRI params */
		return -ENODEV_;
	case I915_PARAM_CHIPSET_ID: value = pci_devid; break;
	case I915_PARAM_REVISION:   value = pci_rev; break;
	case 6:  value = 0; break;                         /* NUM_FENCES_AVAIL: no fence registers used */
	case 7:  value = 0; break;                         /* HAS_OVERLAY */
	case 10: case 11: case 22: case 31: value = 0; break;   /* BSD, BLT, VEBOX, BSD2: render only, for now */
	case 17: value = 1; break;                         /* HAS_LLC */
	case 27: value = 0; break;                         /* HAS_WT */
	case 18: value = 3; break;                         /* HAS_ALIASING_PPGTT: full 48-bit */
	case 20: value = 0; break;                         /* HAS_SEMAPHORES */
	case 23: value = 0; break;                         /* HAS_SECURE_BATCHES */
	case 28: return -ENODEV_;                          /* CMD_PARSER_VERSION: no parser on Gen9 */
	case I915_PARAM_SUBSLICE_TOTAL: value = 3; break;
	case I915_PARAM_EU_TOTAL: value = 24; break;
	case I915_PARAM_HAS_GPU_RESET: value = 2; break;   /* engine reset */
	case 36: value = 0; break;                         /* RESOURCE_STREAMER */
	case 38: case 39: value = 0; break;                /* POOLED_EU */
	case 42: return -ENODEV_;                          /* HUC_STATUS */
	case 40: value = 4; break;                         /* MMAP_GTT_VERSION */
	case I915_PARAM_HAS_SCHEDULER: value = 0; break;
	case 30:                                           /* MMAP_VERSION */
	case 5: case 8: case 9: case 12: case 13: case 15: case 16: case 19: case 21: case 24:
	case 25: case 26: case 29: case 37: case 43: case 44: case 45: case 48: case 49:
		value = 1;                                     /* the "always 1" group */
		break;
	case 53: case 55: case 56: value = 0; break;       /* SUBMIT_FENCE, TIMELINE_FENCES, USERPTR_PROBE */
	case I915_PARAM_HAS_CONTEXT_ISOLATION: value = 1; break;   /* every context restores its registers */
	case I915_PARAM_SLICE_MASK: value = 1; break;
	case I915_PARAM_SUBSLICE_MASK: value = 7; break;
	case I915_PARAM_CS_TIMESTAMP_FREQUENCY: value = 12000000; break;   /* KBL: 12 MHz */
	case 52: value = 1; break;                         /* MMAP_GTT_COHERENT */
	default: return -EINVAL_;
	}
	return copy_to_user((void *)(uintptr_t)g->value, &value, sizeof(value)) ? -EFAULT_ : 0;
}

/* ── contexts ── */

static uctx_t *ctx_lookup(ifile_t *f, uint32_t id)
{
	for (uctx_t *c = f->ctxs; c; c = c->next) if (c->id == id) return c;
	return NULL;
}

static uctx_t *ctx_new(ifile_t *f, uint32_t id)
{
	if (gt_start()) return NULL;
	uctx_t *c = kmalloc(sizeof(*c));
	if (!c) return NULL;
	memset(c, 0, sizeof(*c));
	c->gt[0] = gt_ctx_create(f->vm.pml4);
	if (!c->gt[0]) { kfree(c); return NULL; }
	c->id = id;
	c->bannable = c->recoverable = c->persistence = 1;
	c->next = f->ctxs;
	f->ctxs = c;
	return c;
}

/* every hardware context of c (waits for their requests), then c itself */
static void ctx_free(uctx_t *c)
{
	for (int i = 0; i < 8; i++) gt_ctx_destroy(c->gt[i]);
	kfree(c);
}

/* the hardware context for an execbuf's ring field, or NULL with *e set */
static gt_ctx_t *ctx_engine(ifile_t *f, uctx_t *c, uint32_t ring, int *e)
{
	*e = -EINVAL_;
	if (!c->nengines) return ring > 1 ? NULL : c->gt[0];   /* DEFAULT and RENDER: one rcs0 */
	if (ring >= (uint32_t)c->nengines || c->eng[ring].engine_class != I915_ENGINE_CLASS_RENDER) return NULL;
	if (!c->gt[ring]) c->gt[ring] = gt_ctx_create(f->vm.pml4);
	if (!c->gt[ring]) *e = gt_usable() ? -ENOMEM_ : -EIO_;
	return c->gt[ring];
}

static int ctx_setparam(ifile_t *f, uctx_t *c, struct drm_i915_gem_context_param *p)
{
	switch (p->param) {
	case I915_CONTEXT_PARAM_ENGINES: {
		if (!p->size) { c->nengines = 0; return 0; }
		if (p->size < 8 || (p->size - 8) % 4) return -EINVAL_;
		uint32_t n = (p->size - 8) / 4;
		if (n > 8) return -EINVAL_;
		uint64_t ext;
		if (copy_from_user(&ext, (void *)(uintptr_t)p->value, 8)) return -EFAULT_;
		if (ext) return -ENODEV_;                      /* load balancing, bonds: not yet */
		struct i915_engine_class_instance e[8];
		if (n && copy_from_user(e, (void *)(uintptr_t)(p->value + 8), n * 4)) return -EFAULT_;
		for (uint32_t i = 0; i < n; i++)
			if (!(e[i].engine_class == I915_ENGINE_CLASS_RENDER && e[i].engine_instance == 0) &&
			    e[i].engine_class != 0xffff)
				return -EINVAL_;
		memcpy(c->eng, e, n * 4);
		c->nengines = (int)n;
		for (int i = 1; i < 8; i++) { gt_ctx_destroy(c->gt[i]); c->gt[i] = NULL; }   /* a new map: new slots */
		return 0;
	}
	case I915_CONTEXT_PARAM_PRIORITY:
		if ((int64_t)p->value > 1023 || (int64_t)p->value < -1023) return -EINVAL_;
		c->priority = (int32_t)p->value;
		return 0;
	case I915_CONTEXT_PARAM_VM:
		return p->value == 1 ? 0 : -ENOENT_;           /* one address space per file, id 1 */
	case I915_CONTEXT_PARAM_BANNABLE: case I915_CONTEXT_PARAM_RECOVERABLE:
	case I915_CONTEXT_PARAM_PERSISTENCE: case I915_CONTEXT_PARAM_NO_ERROR_CAPTURE:
		if (p->size || p->value > 1) return -EINVAL_;
		if (p->param == I915_CONTEXT_PARAM_BANNABLE) c->bannable = (uint8_t)p->value;
		else if (p->param == I915_CONTEXT_PARAM_RECOVERABLE) c->recoverable = (uint8_t)p->value;
		else if (p->param == I915_CONTEXT_PARAM_PERSISTENCE) c->persistence = (uint8_t)p->value;
		else c->no_capture = (uint8_t)p->value;
		return 0;
	case I915_CONTEXT_PARAM_RINGSIZE:
		return 0;
	case I915_CONTEXT_PARAM_SSEU:
		return -ENODEV_;
	}
	(void)f;
	return -EINVAL_;
}

static int ioc_ctx_create(drm_filep_t *fp, void *d)
{
	struct drm_i915_gem_context_create_ext *a = d;
	ifile_t *f = ifile(fp);
	if (!f) return -ENOMEM_;
	if (a->flags & ~(I915_CONTEXT_CREATE_FLAGS_USE_EXTENSIONS | I915_CONTEXT_CREATE_FLAGS_SINGLE_TIMELINE)) return -EINVAL_;
	uctx_t *c = ctx_new(f, f->next_ctx);
	if (!c) return gt_usable() ? -ENOMEM_ : -EIO_;
	int e = 0;
	if (a->flags & I915_CONTEXT_CREATE_FLAGS_USE_EXTENSIONS)
		for (uint64_t ext = a->extensions, hops = 0; ext && !e; ) {
			struct drm_i915_gem_context_create_ext_setparam sp;
			if (++hops > MAX_EXT) { e = -E2BIG_; break; }     /* a looping chain must not spin here */
			if (kloop_should_stop()) { e = -EINTR_; break; }
			if (copy_from_user(&sp, (void *)(uintptr_t)ext, sizeof(sp))) { e = -EFAULT_; break; }
			if (sp.base.name != I915_CONTEXT_CREATE_EXT_SETPARAM) { e = -EINVAL_; break; }
			if (sp.param.ctx_id) { e = -EINVAL_; break; }
			e = ctx_setparam(f, c, &sp.param);
			ext = sp.base.next_extension;
		}
	if (e) {
		f->ctxs = c->next;
		ctx_free(c);
		return e;
	}
	a->ctx_id = f->next_ctx++;
	return 0;
}

static int ioc_ctx_destroy(drm_filep_t *fp, void *d)
{
	struct drm_i915_gem_context_destroy *a = d;
	ifile_t *f = ifile(fp);
	if (!f || a->pad || !a->ctx_id) return -EINVAL_;
	for (uctx_t **pp = &f->ctxs; *pp; pp = &(*pp)->next)
		if ((*pp)->id == a->ctx_id) {
			uctx_t *c = *pp;
			*pp = c->next;
			ctx_free(c);
			return 0;
		}
	return -ENOENT_;
}

static int ctx_get(drm_filep_t *fp, uint32_t id, uctx_t **out)
{
	ifile_t *f = ifile(fp);
	if (!f) return -ENOMEM_;
	uctx_t *c = ctx_lookup(f, id);
	if (!c && id == 0) c = ctx_new(f, 0);              /* the default context, on first use */
	if (!c) return id ? -ENOENT_ : (gt_usable() ? -ENOMEM_ : -EIO_);
	*out = c;
	return 0;
}

static int ioc_ctx_setparam(drm_filep_t *fp, void *d)
{
	struct drm_i915_gem_context_param *p = d;
	uctx_t *c;
	int e = ctx_get(fp, p->ctx_id, &c);
	return e ? e : ctx_setparam(fp->driver_priv, c, p);
}

static int ioc_ctx_getparam(drm_filep_t *fp, void *d)
{
	struct drm_i915_gem_context_param *p = d;
	uctx_t *c;
	int e = ctx_get(fp, p->ctx_id, &c);
	if (e) return e;
	switch (p->param) {
	case I915_CONTEXT_PARAM_GTT_SIZE: p->size = 0; p->value = VA_48B; return 0;
	case I915_CONTEXT_PARAM_PRIORITY: p->size = 0; p->value = (uint64_t)(int64_t)c->priority; return 0;
	case I915_CONTEXT_PARAM_VM:       p->size = 0; p->value = 1; return 0;
	case I915_CONTEXT_PARAM_BANNABLE:    p->size = 0; p->value = c->bannable; return 0;
	case I915_CONTEXT_PARAM_RECOVERABLE: p->size = 0; p->value = c->recoverable; return 0;
	case I915_CONTEXT_PARAM_PERSISTENCE: p->size = 0; p->value = c->persistence; return 0;
	case I915_CONTEXT_PARAM_NO_ERROR_CAPTURE: p->size = 0; p->value = c->no_capture; return 0;
	case I915_CONTEXT_PARAM_ENGINES: {
		uint32_t need = c->nengines ? 8 + 4 * (uint32_t)c->nengines : 0;
		if (!p->size) { p->size = need; return 0; }
		if (p->size < need) return -EINVAL_;
		uint64_t zero = 0;
		if (need && (copy_to_user((void *)(uintptr_t)p->value, &zero, 8) ||
		             copy_to_user((void *)(uintptr_t)(p->value + 8), c->eng, need - 8))) return -EFAULT_;
		p->size = need;
		return 0;
	}
	}
	return -EINVAL_;
}

static int ioc_reset_stats(drm_filep_t *fp, void *d)
{
	struct drm_i915_reset_stats *r = d;
	uctx_t *c;
	if (r->flags) return -EINVAL_;
	int e = ctx_get(fp, r->ctx_id, &c);
	if (e) return e;
	r->reset_count = task_current()->euid == 0 ? gt_reset_count() : 0;
	r->batch_active = r->batch_pending = 0;
	return 0;
}

static int ioc_reg_read(drm_filep_t *fp, void *d)
{
	(void)fp;
	struct drm_i915_reg_read *r = d;
	uint64_t reg = r->offset & ~1ULL;
	if (reg != 0x2358) return -EINVAL_;                /* RING_TIMESTAMP(RCS): the one iris reads */
	if (gt_start()) return -EIO_;
	volatile uint8_t *m = intel_mmio();
	uint32_t hi, lo, hi2;
	do {
		hi = *(volatile uint32_t *)(m + reg + 4);
		lo = *(volatile uint32_t *)(m + reg);
		hi2 = *(volatile uint32_t *)(m + reg + 4);
	} while (hi != hi2);
	r->val = (uint64_t)hi << 32 | lo;
	return 0;
}

static int ioc_userptr(drm_filep_t *fp, void *d) { (void)fp; (void)d; return -ENODEV_; }
static int ioc_vm_create(drm_filep_t *fp, void *d) { (void)fp; (void)d; return -ENODEV_; }

/* ── QUERY ── */

static int query_put(struct drm_i915_query_item *it, const void *data, int32_t len)
{
	if (it->length == 0) { it->length = len; return 0; }
	if (it->length < len) { it->length = -EINVAL_; return 0; }
	if (copy_to_user((void *)(uintptr_t)it->data_ptr, data, (size_t)len)) return -EFAULT_;
	it->length = len;
	return 0;
}

static int ioc_query(drm_filep_t *fp, void *d)
{
	(void)fp;
	struct drm_i915_query *q = d;
	if (q->flags || q->num_items > 256) return -EINVAL_;
	for (uint32_t i = 0; i < q->num_items; i++) {
		if (kloop_should_stop()) return -EINTR_;
		struct drm_i915_query_item it;
		uint64_t up = q->items_ptr + i * sizeof(it);
		if (copy_from_user(&it, (void *)(uintptr_t)up, sizeof(it))) return -EFAULT_;
		int e = 0;
		if (it.flags && it.query_id != DRM_I915_QUERY_TOPOLOGY_INFO) it.length = -EINVAL_;
		else switch (it.query_id) {
		case DRM_I915_QUERY_TOPOLOGY_INFO: {        /* KBL GT2: 1 slice, 3 subslices, 8 EUs each */
			uint8_t buf[sizeof(struct drm_i915_query_topology_info) + 5];
			struct drm_i915_query_topology_info *t = (void *)buf;
			memset(buf, 0, sizeof(buf));
			t->max_slices = 1; t->max_subslices = 3; t->max_eus_per_subslice = 8;
			t->subslice_offset = 1; t->subslice_stride = 1;
			t->eu_offset = 2; t->eu_stride = 1;
			t->data[0] = 0x1; t->data[1] = 0x7; t->data[2] = t->data[3] = t->data[4] = 0xff;
			e = query_put(&it, buf, (int32_t)sizeof(buf));
			break;
		}
		case DRM_I915_QUERY_ENGINE_INFO: {
			uint64_t buf[(sizeof(struct drm_i915_query_engine_info) + sizeof(struct drm_i915_engine_info)) / 8];
			struct drm_i915_query_engine_info *ei = (void *)buf;
			memset(buf, 0, sizeof(buf));
			ei->num_engines = 1;
			ei->engines[0].engine.engine_class = I915_ENGINE_CLASS_RENDER;
			ei->engines[0].flags = 1;                  /* HAS_LOGICAL_INSTANCE, instance 0 */
			e = query_put(&it, buf, (int32_t)sizeof(buf));
			break;
		}
		case DRM_I915_QUERY_MEMORY_REGIONS: {
			uint64_t buf[(sizeof(struct drm_i915_query_memory_regions) + sizeof(struct drm_i915_memory_region_info)) / 8];
			struct drm_i915_query_memory_regions *mr = (void *)buf;
			memset(buf, 0, sizeof(buf));
			mr->num_regions = 1;                       /* system memory, class 0 instance 0 */
			mr->regions[0].probed_size = buddy_total_pages() * PAGE_SIZE;
			mr->regions[0].unallocated_size = buddy_free_pages() * PAGE_SIZE;
			e = query_put(&it, buf, (int32_t)sizeof(buf));
			break;
		}
		default:
			it.length = -EINVAL_;
		}
		if (e) return e;
		if (copy_to_user((void *)(uintptr_t)up, &it, sizeof(it))) return -EFAULT_;
	}
	return 0;
}

/* ── EXECBUFFER2 ── */

static uint64_t canonical(uint64_t a) { return (uint64_t)((int64_t)(a << 16) >> 16); }

static void unbind(ifile_t *f, uint32_t h, drm_gem_t *o)
{
	uint64_t *v = h <= f->nva ? &f->va[h - 1] : NULL;
	if (!v || !(*v & BOUND)) return;
	if (gt_seqno_busy(o->gpu_seqno)) obj_wait(o, 2000000000LL);
	ppgtt_unmap(&f->vm, *v & ~BOUND, o->size);
	*v = 0;
}

/* Another handle mapped over [va, va+size) loses its mapping first, as i915 evicts
 * an overlapping vma: otherwise closing it later would clear the new object's PTEs
 * (seen on Casper: glmark2 phong read garbage and hung the engine). */
static void evict_overlaps(drm_filep_t *fp, ifile_t *f, uint32_t self, uint64_t va, uint64_t size)
{
	for (uint32_t i = 0; i < f->nva; i++) {
		if (!(f->va[i] & BOUND) || i + 1 == self) continue;
		uint64_t ov = f->va[i] & ~BOUND;
		drm_gem_t *oo = drm_gem_lookup(fp, i + 1);
		uint64_t osz = oo ? oo->size : PAGE_SIZE;
		if (ov < va + size && va < ov + osz) {
			ppgtt_unmap(&f->vm, ov, osz);
			f->va[i] = 0;
		}
	}
}

static int bind(drm_filep_t *fp, ifile_t *f, uint32_t h, drm_gem_t *o, struct drm_i915_gem_exec_object2 *x)
{
	uint64_t *v = va_slot(f, h);
	if (!v) return -ENOMEM_;
	int b48 = (x->flags & EXEC_OBJECT_SUPPORTS_48B_ADDRESS) != 0;
	uint64_t want;
	if (x->flags & EXEC_OBJECT_PINNED) {
		want = x->offset & (VA_48B - 1);
		if (want & (PAGE_SIZE - 1)) return -EINVAL_;
		if (!b48 && want + o->size > VA_HIGH) return -EINVAL_;
		if ((*v & BOUND) && (*v & ~BOUND) == want) return 0;
	} else {
		if (*v & BOUND) {
			uint64_t cur = *v & ~BOUND;
			if (b48 || cur + o->size <= VA_HIGH) { x->offset = canonical(cur); return 0; }
		}
		uint64_t align = x->alignment > PAGE_SIZE ? x->alignment : PAGE_SIZE;
		if (align & (align - 1)) return -EINVAL_;
		uint64_t *next = b48 ? &f->hi_next : &f->lo_next;
		want = (*next + align - 1) & ~(align - 1);
		if (!b48 && want + o->size > VA_HIGH) return -ENOSPC_;
		*next = want + o->size + PAGE_SIZE;             /* a guard page between objects */
	}
	unbind(f, h, o);
	evict_overlaps(fp, f, h, want, o->size);
	int e = ppgtt_map(&f->vm, want, o);
	if (e) return e;
	*v = want | BOUND;
	x->offset = canonical(want);
	return 0;
}

static int do_relocs(drm_filep_t *fp, ifile_t *f, struct drm_i915_gem_exec_object2 *x, drm_gem_t **objs,
                     uint32_t count, uint32_t i, uint64_t flags)
{
	drm_gem_t *o = objs[i];
	if (x[i].relocation_count > o->size / 4) return -EINVAL_;   /* more entries than dwords to patch */
	for (uint32_t r = 0; r < x[i].relocation_count; r++) {
		if (kloop_should_stop()) return -EINTR_;
		struct drm_i915_gem_relocation_entry re;
		uint64_t up = x[i].relocs_ptr + r * sizeof(re);
		if (copy_from_user(&re, (void *)(uintptr_t)up, sizeof(re))) return -EFAULT_;
		uint32_t th;
		if (flags & I915_EXEC_HANDLE_LUT) {
			if (re.target_handle >= count) return -EINVAL_;
			th = x[re.target_handle].handle;
		} else th = re.target_handle;
		uint64_t *tv = th <= f->nva ? &f->va[th - 1] : NULL;
		if (!th || !tv || !(*tv & BOUND) || !drm_gem_lookup(fp, th)) return -ENOENT_;
		uint64_t target = canonical((*tv & ~BOUND) + (int64_t)(int32_t)re.delta);
		if ((flags & I915_EXEC_NO_RELOC) && re.presumed_offset == canonical(*tv & ~BOUND)) continue;
		if ((re.offset & 3) || re.offset + 8 > o->size) return -EINVAL_;
		uint64_t av;
		uint8_t *p = drm_gem_vaddr(o, re.offset, &av);
		if (av >= 8) *(uint64_t *)p = target;
		else for (int k = 0; k < 8; k++) *(uint8_t *)drm_gem_vaddr(o, re.offset + k, NULL) = (uint8_t)(target >> (8 * k));
		re.presumed_offset = canonical(*tv & ~BOUND);
		if (copy_to_user((void *)(uintptr_t)up, &re, sizeof(re))) return -EFAULT_;
	}
	return 0;
}

static int ioc_execbuffer2(drm_filep_t *fp, void *d)
{
	struct drm_i915_gem_execbuffer2 *eb = d;
	if (eb->flags & ~I915_EXEC_KNOWN_FLAGS) return -EINVAL_;
	if (eb->flags & (I915_EXEC_USE_EXTENSIONS | I915_EXEC_FENCE_SUBMIT)) return -EINVAL_;
	if ((eb->flags & I915_EXEC_SECURE) && task_current()->euid != 0) return -EPERM_;
	if (!eb->buffer_count || eb->buffer_count > 4096) return -EINVAL_;
	if (eb->batch_start_offset & 7) return -EINVAL_;

	uint32_t ctx_id = (uint32_t)eb->rsvd1;
	uctx_t *c;
	int e;
	if (mutex_lock_killable(&drm_lock)) return -EINTR_;
	ifile_t *f = ifile(fp);
	if (!f) { mutex_unlock(&drm_lock); return -ENOMEM_; }
	if ((e = ctx_get(fp, ctx_id, &c))) { mutex_unlock(&drm_lock); return e; }
	gt_ctx_t *gc = ctx_engine(f, c, (uint32_t)(eb->flags & I915_EXEC_RING_MASK), &e);
	if (!gc) { mutex_unlock(&drm_lock); return e; }

	uint32_t n = eb->buffer_count;
	struct drm_i915_gem_exec_object2 *x = kmalloc(n * sizeof(*x));
	drm_gem_t **objs = kmalloc(n * sizeof(*objs));
	if (!x || !objs) { kfree(x); kfree(objs); mutex_unlock(&drm_lock); return -ENOMEM_; }
	e = copy_from_user(x, (void *)(uintptr_t)eb->buffers_ptr, n * sizeof(*x)) ? -EFAULT_ : 0;
	for (uint32_t i = 0; !e && i < n; i++) {
		if (kloop_should_stop()) { e = -EINTR_; break; }       /* n up to 4096, the scan below n^2/2 */
		if (x[i].flags & ~(uint64_t)EXEC_OBJECT_KNOWN_FLAGS) { e = -EINVAL_; break; }
		objs[i] = drm_gem_lookup(fp, x[i].handle);
		if (!objs[i]) { e = -ENOENT_; break; }
		for (uint32_t j = 0; j < i; j++) if (x[j].handle == x[i].handle) { e = -EINVAL_; break; }
	}
	for (uint32_t i = 0; !e && i < n; i++) e = kloop_should_stop() ? -EINTR_ : bind(fp, f, x[i].handle, objs[i], &x[i]);
	for (uint32_t i = 0; !e && i < n; i++) if (x[i].relocation_count) e = do_relocs(fp, f, x, objs, n, i, eb->flags);

	/* fences in: sync_files and syncobjs here are always signaled, or absent */
	struct drm_i915_gem_exec_fence *fen = NULL;
	uint32_t nfen = 0;
	if (!e && (eb->flags & I915_EXEC_FENCE_ARRAY)) {
		nfen = eb->num_cliprects;
		if (nfen > 1024) e = -EINVAL_;
		else if (nfen) {
			fen = kmalloc(nfen * sizeof(*fen));
			if (!fen) e = -ENOMEM_;
			else if (copy_from_user(fen, (void *)(uintptr_t)eb->cliprects_ptr, nfen * sizeof(*fen))) e = -EFAULT_;
			for (uint32_t i = 0; !e && i < nfen; i++) {
				if (kloop_should_stop()) { e = -EINTR_; break; }
				drm_syncobj_t *s = drm_syncobj_find(fp, fen[i].handle);
				if (!s) e = -ENOENT_;
				else if ((fen[i].flags & I915_EXEC_FENCE_WAIT) && !s->signaled) e = -EINVAL_;
			}
		}
	}
	if (!e && (eb->flags & I915_EXEC_FENCE_IN)) {
		vfs_file_t *ff = fget(task_current()->fds, (int)(uint32_t)eb->rsvd2);
		if (!ff) e = -EINVAL_;
		else { if (!drm_is_syncfile(ff)) e = -EINVAL_; fput(ff); }
	}

	uint32_t bi = (eb->flags & I915_EXEC_BATCH_FIRST) ? 0 : n - 1;
	uint32_t seq = 0;
	if (!e) {
		uint64_t batch = (*va_slot(f, x[bi].handle) & ~BOUND) + eb->batch_start_offset;
		if (eb->batch_start_offset >= objs[bi]->size) e = -EINVAL_;
		else e = gt_submit(gc, batch, &seq);
	}
	if (!e) {
		for (uint32_t i = 0; i < n; i++) objs[i]->gpu_seqno = seq;
		if (copy_to_user((void *)(uintptr_t)eb->buffers_ptr, x, n * sizeof(*x))) e = -EFAULT_;
	}
	int must_wait = !e && ((eb->flags & I915_EXEC_FENCE_OUT) || fen);
	mutex_unlock(&drm_lock);

	if (must_wait) {                       /* signal only finished work: wait for it */
		int we = gt_wait_seqno(seq, -1);
		mutex_lock(&drm_lock);
		for (uint32_t i = 0; i < nfen; i++)
			if (fen[i].flags & I915_EXEC_FENCE_SIGNAL) {
				drm_syncobj_t *s = drm_syncobj_find(fp, fen[i].handle);
				if (s && !we) drm_syncobj_signal(s, 0);
			}
		mutex_unlock(&drm_lock);
		if (!we && (eb->flags & I915_EXEC_FENCE_OUT)) {
			int fd = drm_syncfile_new();
			if (fd < 0) e = fd;
			else eb->rsvd2 = (eb->rsvd2 & 0xffffffffULL) | ((uint64_t)(uint32_t)fd << 32);
		}
		if (we == -EIO_) e = -EIO_;
	}
	kfree(fen);
	kfree(x);
	kfree(objs);
	return e;
}

/* ── per-file teardown hooks (under drm_lock) ── */

static void i915_handle_close(drm_filep_t *fp, uint32_t h, drm_gem_t *o)
{
	ifile_t *f = fp->driver_priv;
	if (f) unbind(f, h, o);
}

static void i915_close(drm_filep_t *fp)
{
	ifile_t *f = fp->driver_priv;
	if (!f) return;
	while (f->ctxs) {
		uctx_t *c = f->ctxs;
		f->ctxs = c->next;
		ctx_free(c);                           /* waits for its requests */
	}
	ppgtt_fini(&f->vm);
	kfree(f->va);
	kfree(f);
	fp->driver_priv = NULL;
}

/* ── the table ── */

#define I(nr, st, fl, fn) [DRM_I915_##nr] = { sizeof(st), (fl) | F_RENDER, fn }
static const drm_ioctl_desc_t i915_ioctls[DRM_I915_NIOCTLS] = {
	I(GETPARAM,          struct drm_i915_getparam, 0, ioc_getparam),
	I(GEM_BUSY,          struct drm_i915_gem_busy, 0, ioc_busy),
	I(GEM_THROTTLE,      uint32_t, 0, ioc_throttle),
	I(GEM_CREATE,        struct drm_i915_gem_create, 0, ioc_gem_create),
	I(GEM_PREAD,         struct drm_i915_gem_pread, F_NOLOCK, ioc_pread),
	I(GEM_PWRITE,        struct drm_i915_gem_pwrite, F_NOLOCK, ioc_pwrite),
	I(GEM_MMAP,          uint64_t[5], 0, ioc_mmap_legacy),
	I(GEM_SET_DOMAIN,    struct drm_i915_gem_set_domain, F_NOLOCK, ioc_set_domain),
	I(GEM_SW_FINISH,     struct drm_i915_gem_sw_finish, 0, ioc_sw_finish),
	I(GEM_SET_TILING,    struct drm_i915_gem_set_tiling, 0, ioc_set_tiling),
	I(GEM_GET_TILING,    struct drm_i915_gem_get_tiling, 0, ioc_get_tiling),
	I(GEM_GET_APERTURE,  struct drm_i915_gem_get_aperture, 0, ioc_aperture),
	I(GEM_MMAP_OFFSET,   struct drm_i915_gem_mmap_offset, 0, ioc_mmap_offset),
	I(GEM_MADVISE,       struct drm_i915_gem_madvise, 0, ioc_madvise),
	I(GEM_EXECBUFFER2,   struct drm_i915_gem_execbuffer2, F_NOLOCK, ioc_execbuffer2),
	I(GEM_WAIT,          struct drm_i915_gem_wait, F_NOLOCK, ioc_wait),
	I(GEM_CONTEXT_CREATE, struct drm_i915_gem_context_create_ext, 0, ioc_ctx_create),
	I(GEM_CONTEXT_DESTROY, struct drm_i915_gem_context_destroy, 0, ioc_ctx_destroy),
	I(GEM_SET_CACHING,   struct drm_i915_gem_caching, 0, ioc_set_caching),
	I(GEM_GET_CACHING,   struct drm_i915_gem_caching, 0, ioc_get_caching),
	I(REG_READ,          struct drm_i915_reg_read, 0, ioc_reg_read),
	I(GET_RESET_STATS,   struct drm_i915_reset_stats, 0, ioc_reset_stats),
	I(GEM_USERPTR,       uint64_t[3], 0, ioc_userptr),
	I(GEM_CONTEXT_GETPARAM, struct drm_i915_gem_context_param, 0, ioc_ctx_getparam),
	I(GEM_CONTEXT_SETPARAM, struct drm_i915_gem_context_param, 0, ioc_ctx_setparam),
	I(QUERY,             struct drm_i915_query, 0, ioc_query),
	I(GEM_VM_CREATE,     uint64_t[2], 0, ioc_vm_create),
	I(GEM_VM_DESTROY,    uint64_t[2], 0, ioc_vm_create),
	I(GEM_CREATE_EXT,    struct drm_i915_gem_create_ext, 0, ioc_gem_create_ext),
};

static const drm_driver_t i915_driver = {
	.ioctls = i915_ioctls,
	.nioctls = DRM_I915_NIOCTLS,
	.close = i915_close,
	.handle_close = i915_handle_close,
};

const drm_driver_t *ranix_i915_init(void)
{
	pci_devid = pci_read16(drm_pci_bus, drm_pci_dev, drm_pci_fn, PCI_DEVICE_ID);
	pci_rev = (uint16_t)(pci_read32(drm_pci_bus, drm_pci_dev, drm_pci_fn, 0x08) & 0xff);
	/* gt_tick runs from drm_kms_tick: one tick callback for all of DRM (registering
	 * at run time grows the scheduler's callback array under a running tick) */
	return &i915_driver;
}
