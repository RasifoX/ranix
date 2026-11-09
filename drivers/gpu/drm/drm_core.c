/*
 * Ranix DRM core: /dev/dri/card0 and /dev/dri/renderD128, the ioctl dispatcher,
 * per-open state (GEM handles, syncobjs, the event queue), GEM objects with dumb
 * buffers and mmap, PRIME (dma-buf fds), syncobj with sync_file export.
 *
 * Written for Ranix from the published uAPI (Linux include/uapi/drm) and libdrm's
 * behaviour; no Linux code. Display (KMS) lives in drm_kms.c.
 *
 * Locking: drm_lock (a sleeping mutex) around every ioctl and every object
 * refcount; drm_ev_lock (IRQ-safe) around event queues, which the 1 ms tick fills.
 */
#include <kernel/vfs.h>
#include <kernel/fs/devfs.h>
#include <kernel/mm/heap.h>
#include <kernel/mm/buddy.h>
#include <kernel/mm/pmm.h>
#include <kernel/lib/string.h>
#include <kernel/proc.h>
#include <kernel/hal.h>
#include <kernel/kprintf.h>
#include <kernel/tick.h>
#include <kernel/sync.h>
#include <kernel/pci.h>
#include "drm_priv.h"

mutex_t      drm_lock    = MUTEX_INIT;
spinlock_t   drm_ev_lock = SPINLOCK_INIT;
drm_filep_t *drm_master;
char         drm_driver_name[16] = "simpledrm";

int     drm_pci_found;
uint8_t drm_pci_bus, drm_pci_dev, drm_pci_fn;

static int        drm_ready;
static drm_gem_t *gem_all;            /* every live object, for flink names */
static uint32_t   gem_next_name = 1;
static uint64_t   gem_next_off  = 0x10000000ULL;   /* fake mmap offsets, never reused */
static uint32_t   drm_next_magic = 1;

#define EVENT_SPACE 4096
#define SYNCOBJ_MAX 256

uint64_t drm_now_ns(void) { return mono_ns(); }

void drm_sleep_ms(uint32_t ms) { task_sleep_until(kernel_ticks + ms); }   /* 1 kHz tick */

/* Write the CPU's cache lines for [p, p+len) back to memory: the display and the
 * GPU's GGTT walks do not snoop. CLFLUSHOPT (Skylake+) pipelines where CLFLUSH
 * serialises. */
void drm_cache_flush(void *p, uint64_t len)
{
#ifdef __x86_64__
	static int has_clflushopt = -1;
	if (has_clflushopt < 0) {
		uint32_t a = 7, b, c = 0, d;
		__asm__ volatile("cpuid" : "+a"(a), "=b"(b), "+c"(c), "=d"(d));
		has_clflushopt = (b >> 23) & 1;
	}
	uint8_t *q = (uint8_t *)((uintptr_t)p & ~63UL), *end = (uint8_t *)p + len;
	__asm__ volatile("mfence" ::: "memory");
	if (has_clflushopt)
		for (; q < end; q += 64) __asm__ volatile("clflushopt (%0)" :: "r"(q) : "memory");
	else
		for (; q < end; q += 64) __asm__ volatile("clflush (%0)" :: "r"(q) : "memory");
	__asm__ volatile("mfence" ::: "memory");
#else
	(void)p; (void)len;
#endif
}

/* the same for part of an object, page by page (its pages are not contiguous) */
void drm_gem_flush(drm_gem_t *o, uint64_t off, uint64_t len)
{
	uint64_t end = off + len > o->size ? o->size : off + len;
	for (uint64_t a = off; a < end; ) {
		uint64_t av;
		uint8_t *p = drm_gem_vaddr(o, a, &av);   /* the rest of this page */
		uint64_t n = a + av < end ? av : end - a;
		drm_cache_flush(p, n);
		a += n;
	}
}

/* ── GEM objects ─────────────────────────────────────────────── */

drm_gem_t *drm_gem_alloc(uint64_t size)
{
	size = (size + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
	if (!size || size > (1ULL << 31)) return NULL;
	uint64_t n = size / PAGE_SIZE;
	if (n > buddy_free_pages() / 2) return NULL;     /* leave the system its memory */
	drm_gem_t *o = kmalloc(sizeof(*o));
	if (!o) return NULL;
	memset(o, 0, sizeof(*o));
	o->pages = kmalloc(n * sizeof(uint64_t));
	if (!o->pages) { kfree(o); return NULL; }
	for (uint64_t i = 0; i < n; i++) {
		void *pg = buddy_alloc(0);
		if (!pg) {
			while (i--) pmm_free((void *)o->pages[i]);
			kfree(o->pages); kfree(o);
			return NULL;
		}
		memset((void *)PHYS_TO_VIRT((uint64_t)pg), 0, PAGE_SIZE);
		o->pages[i] = (uint64_t)pg;
	}
	o->npages   = (uint32_t)n;
	o->size     = size;
	o->refcount = 1;
	o->next     = gem_all;
	gem_all     = o;
	return o;
}

void drm_gem_get(drm_gem_t *o) { if (o) o->refcount++; }

void drm_gem_put(drm_gem_t *o)
{
	if (!o || --o->refcount) return;
	drm_kms_gem_release(o);                 /* off the display, out of the GGTT */
	for (drm_gem_t **pp = &gem_all; *pp; pp = &(*pp)->next)
		if (*pp == o) { *pp = o->next; break; }
	for (uint32_t i = 0; i < o->npages; i++) pmm_free((void *)o->pages[i]);
	kfree(o->pages);
	kfree(o);
}

/* kernel address of byte off, and how many bytes follow it in the same page */
void *drm_gem_vaddr(drm_gem_t *o, uint64_t off, uint64_t *avail)
{
	if (off >= o->size) { if (avail) *avail = 0; return NULL; }
	uint64_t in = off & (PAGE_SIZE - 1);
	if (avail) *avail = PAGE_SIZE - in;
	return (void *)(PHYS_TO_VIRT(o->pages[off / PAGE_SIZE]) + in);
}

drm_gem_t *drm_gem_lookup(drm_filep_t *fp, uint32_t h)
{
	if (!h || h > fp->nhandles) return NULL;
	return fp->handles[h - 1];
}

/* the handle this file already has for o, else 0 */
static uint32_t gem_handle_of(drm_filep_t *fp, drm_gem_t *o)
{
	for (uint32_t i = 0; i < fp->nhandles; i++)
		if (fp->handles[i] == o) return i + 1;
	return 0;
}

static int slot_grow(void ***arr, uint32_t *n)
{
	uint32_t nn = *n ? *n * 2 : 16;
	if (nn > 65536) return -ENOSPC_;
	void **na = kmalloc(nn * sizeof(void *));
	if (!na) return -ENOMEM_;
	memset(na, 0, nn * sizeof(void *));
	if (*arr) { memcpy(na, *arr, *n * sizeof(void *)); kfree(*arr); }
	*arr = na;
	*n = nn;
	return 0;
}

/* takes a new reference for the handle */
int drm_gem_handle_new(drm_filep_t *fp, drm_gem_t *o, uint32_t *handle)
{
	for (;;) {
		for (uint32_t i = 0; i < fp->nhandles; i++)
			if (!fp->handles[i]) {
				fp->handles[i] = o;
				drm_gem_get(o);
				*handle = i + 1;
				return 0;
			}
		int e = slot_grow((void ***)&fp->handles, &fp->nhandles);
		if (e) return e;
	}
}

static int gem_handle_delete(drm_filep_t *fp, uint32_t h)
{
	drm_gem_t *o = drm_gem_lookup(fp, h);
	if (!o) return -EINVAL_;
	if (drm_drv && drm_drv->handle_close) drm_drv->handle_close(fp, h, o);
	fp->handles[h - 1] = NULL;
	drm_gem_put(o);
	return 0;
}

/* ── events ──────────────────────────────────────────────────── */

int drm_event_space_take(drm_filep_t *fp, uint32_t len)
{
	uint64_t f = spin_lock_irq(&drm_ev_lock);
	int ok = fp->ev_space >= len;
	if (ok) fp->ev_space -= len;
	spin_unlock_irq(&drm_ev_lock, f);
	return ok ? 0 : -ENOMEM_;
}

/* under drm_ev_lock: into the file's read queue, wake readers and pollers */
void drm_event_deliver(drm_pending_t *p)
{
	drm_filep_t *fp = p->file;
	p->next = NULL;
	if (fp->evq_tail) fp->evq_tail->next = p; else fp->evq = p;
	fp->evq_tail = p;
	wake_up(&fp->wq);
	poll_notify();
}

/* an event that will never be delivered: give its space back */
void drm_event_free_unqueued(drm_pending_t *p)
{
	uint64_t f = spin_lock_irq(&drm_ev_lock);
	p->file->ev_space += p->len;
	spin_unlock_irq(&drm_ev_lock, f);
	kfree(p);
}

static ssize_t drm_read(vfs_file_t *f, void *buf, size_t len)
{
	drm_filep_t *fp = f->priv;
	if (!fp) return -EBADF_;
	uint64_t fl;
	for (;;) {
		fl = spin_lock_irq(&drm_ev_lock);
		if (fp->evq) break;
		if (f->flags & O_NONBLOCK) { spin_unlock_irq(&drm_ev_lock, fl); return -EAGAIN_; }
		sleep_on_locked(&fp->wq, &drm_ev_lock, fl);
		if (task_has_actionable_signal()) return -EINTR_;
	}
	/* take what fits, then copy with the lock dropped */
	drm_pending_t *take = NULL, **tail = &take;
	size_t total = 0;
	while (fp->evq && total + fp->evq->len <= len) {
		drm_pending_t *p = fp->evq;
		fp->evq = p->next;
		if (!fp->evq) fp->evq_tail = NULL;
		total += p->len;
		*tail = p; tail = &p->next; p->next = NULL;
	}
	spin_unlock_irq(&drm_ev_lock, fl);
	if (!take) return -EINVAL_;          /* buffer smaller than one event */
	size_t done = 0;
	int fault = 0;
	while (take) {
		drm_pending_t *p = take;
		take = p->next;
		if (!fault && copy_fop_to_user((char *)buf + done, &p->ev, p->len) != 0) fault = 1;
		else if (!fault) done += p->len;
		fl = spin_lock_irq(&drm_ev_lock);
		fp->ev_space += p->len;
		spin_unlock_irq(&drm_ev_lock, fl);
		kfree(p);
	}
	return done ? (ssize_t)done : -EFAULT_;
}

static int drm_poll(vfs_file_t *f)
{
	drm_filep_t *fp = f->priv;
	int r = 0x0004 | 0x0100;                     /* POLLOUT | POLLWRNORM */
	if (fp && fp->evq) r |= 0x0001 | 0x0040;     /* POLLIN | POLLRDNORM */
	return r;
}

/* ── anonymous fds: dma-buf, syncobj, sync_file ─────────────────── */

static int anon_fd(vfs_fops_t *fops, void *priv, uint64_t ino, int flags)
{
	vfs_inode_t *in = kmalloc(sizeof(*in));
	if (!in) return -ENOMEM_;
	memset(in, 0, sizeof(*in));
	in->ino      = ino;
	in->mode     = 0600;
	in->nlink    = 1;
	in->refcount = 1;
	in->fops     = fops;
	in->priv     = priv;
	vfs_file_t *f = kmalloc(sizeof(*f));
	if (!f) { kfree(in); return -ENOMEM_; }
	memset(f, 0, sizeof(*f));
	f->inode    = in;
	f->flags    = (flags & DRM_RDWR) ? O_RDWR : 0;
	f->refcount = 0;                         /* fd_alloc takes the reference */
	process_t *p = task_current();
	int fd = fd_alloc(p->fds, f);
	if (fd < 0) { kfree(f); kfree(in); return fd; }
	if (flags & DRM_CLOEXEC) fd_set_cloexec(p->fds, fd, 1);
	return fd;
}

/* dma-buf: the object's pages, mappable and passable between processes */
static vfs_fops_t dmabuf_fops;
static vfs_fops_t syncobj_fops;
static vfs_fops_t syncfile_fops;

static uint64_t dmabuf_mmap_page(vfs_file_t *f, off_t off, int sw)
{
	(void)sw;
	mutex_lock(&drm_lock);
	drm_gem_t *o = f->inode->priv;
	uint64_t phys = 0;
	if (o && off >= 0 && (uint64_t)off < o->size) {
		phys = o->pages[(uint64_t)off / PAGE_SIZE];
		pmm_hold((void *)phys);
	}
	mutex_unlock(&drm_lock);
	return phys;
}

static off_t dmabuf_seek(vfs_file_t *f, off_t off, int whence)
{
	drm_gem_t *o = f->inode->priv;
	off_t np;
	if (whence == 0) np = off;
	else if (whence == 1) np = f->pos + off;
	else if (whence == 2) np = (off_t)(o ? o->size : 0) + off;
	else return -EINVAL_;
	if (np < 0) return -EINVAL_;
	f->pos = np;
	return np;
}

static int dmabuf_close(vfs_file_t *f)
{
	mutex_lock(&drm_lock);
	drm_gem_put(f->inode->priv);
	f->inode->priv = NULL;               /* the inode is freed with the file */
	mutex_unlock(&drm_lock);
	return 0;
}

static int syncfile_new(int flags);

static int dmabuf_ioctl(vfs_file_t *f, uint32_t cmd, uint64_t arg)
{
	(void)f;
	switch (cmd) {
	case DMA_BUF_IOCTL_SYNC: {
		uint64_t fl;
		if (copy_from_user(&fl, (void *)(uintptr_t)arg, 8)) return -EFAULT_;
		if (fl & ~7ULL) return -EINVAL_;  /* READ|WRITE|END: CPU-coherent, nothing to do */
		return 0;
	}
	case DMA_BUF_SET_NAME_A: case DMA_BUF_SET_NAME_B:
		return 0;
	case DMA_BUF_IOCTL_EXPORT_SYNC_FILE: {   /* no GPU work: every fence is signaled */
		struct { uint32_t flags; int32_t fd; } a;
		if (copy_from_user(&a, (void *)(uintptr_t)arg, sizeof(a))) return -EFAULT_;
		if (!(a.flags & 3) || (a.flags & ~3u)) return -EINVAL_;
		int fd = syncfile_new(DRM_CLOEXEC);
		if (fd < 0) return fd;
		a.fd = fd;
		if (copy_to_user((void *)(uintptr_t)arg, &a, sizeof(a))) return -EFAULT_;
		return 0;
	}
	case DMA_BUF_IOCTL_IMPORT_SYNC_FILE:
		return 0;
	}
	return -ENOTTY_;
}

static int dmabuf_poll(vfs_file_t *f) { (void)f; return 0x0001 | 0x0004; }

static vfs_fops_t dmabuf_fops = {
	.mmap_page = dmabuf_mmap_page,
	.seek      = dmabuf_seek,
	.close     = dmabuf_close,
	.ioctl     = dmabuf_ioctl,
	.poll      = dmabuf_poll,
};

/* ── the card / render fd ────────────────────────────────────── */

static int drm_open(vfs_inode_t *in, vfs_file_t *f, int flags)
{
	(void)flags;
	if (!drm_ready) return -ENODEV_;
	drm_filep_t *fp = kmalloc(sizeof(*fp));
	if (!fp) return -ENOMEM_;
	memset(fp, 0, sizeof(*fp));
	fp->render   = (in->rdev & 0xff) >= DRM_MINOR_RENDER;
	fp->ev_space = EVENT_SPACE;
	wait_queue_init(&fp->wq);
	if (mutex_lock_killable(&drm_lock)) { kfree(fp); return -EINTR_; }
	if (!fp->render && !drm_master) drm_master = fp;   /* first opener of the card */
	mutex_unlock(&drm_lock);
	f->priv = fp;
	return 0;
}

static void syncobj_put(drm_syncobj_t *s) { if (s && !--s->refcount) kfree(s); }

static int drm_close(vfs_file_t *f)
{
	drm_filep_t *fp = f->priv;
	if (!fp) return 0;
	mutex_lock(&drm_lock);
	if (drm_drv && drm_drv->close) drm_drv->close(fp);   /* contexts, address space: before the handles go */
	drm_kms_file_close(fp);                  /* its framebuffers, its pending vblank events */
	uint64_t fl = spin_lock_irq(&drm_ev_lock);
	drm_pending_t *q = fp->evq;
	fp->evq = fp->evq_tail = NULL;
	spin_unlock_irq(&drm_ev_lock, fl);
	while (q) { drm_pending_t *n = q->next; kfree(q); q = n; }
	for (uint32_t i = 0; i < fp->nhandles; i++) drm_gem_put(fp->handles[i]);
	kfree(fp->handles);
	for (uint32_t i = 0; i < fp->nsyncobjs; i++) syncobj_put(fp->syncobjs[i]);
	kfree(fp->syncobjs);
	if (drm_master == fp) drm_master = NULL;
	mutex_unlock(&drm_lock);
	f->priv = NULL;
	kfree(fp);
	return 0;
}

/* dumb buffers on the card fd: offset from MAP_DUMB, the page with a ref for the map */
static uint64_t drm_mmap_page(vfs_file_t *f, off_t off, int sw)
{
	(void)sw;
	drm_filep_t *fp = f->priv;
	if (!fp || off < 0) return 0;
	uint64_t phys = 0, u = (uint64_t)off;
	mutex_lock(&drm_lock);
	for (uint32_t i = 0; i < fp->nhandles; i++) {
		drm_gem_t *o = fp->handles[i];
		if (o && o->mmap_off && u >= o->mmap_off && u < o->mmap_off + o->size) {
			phys = o->pages[(u - o->mmap_off) / PAGE_SIZE];
			pmm_hold((void *)phys);
			break;
		}
	}
	mutex_unlock(&drm_lock);
	return phys;
}

/* ── core ioctls ─────────────────────────────────────────────── */

static int put_str(uint64_t uptr, uint64_t *ulen, const char *s)
{
	uint64_t n = strlen(s), c = *ulen < n ? *ulen : n;
	if (c && uptr && copy_to_user((void *)(uintptr_t)uptr, s, c)) return -EFAULT_;
	*ulen = n;
	return 0;
}

static int ioc_version(drm_filep_t *fp, void *d)
{
	(void)fp;
	struct drm_version *v = d;
	const drm_display_t *hw = drm_display();
	v->version_major = 1;
	v->version_minor = hw ? hw->version_minor : 0;
	v->version_patchlevel = 0;
	int e;
	if ((e = put_str(v->name, &v->name_len, drm_driver_name))) return e;
	if ((e = put_str(v->date, &v->date_len, "20261108"))) return e;
	return put_str(v->desc, &v->desc_len, hw ? hw->desc : "Ranix firmware framebuffer");
}

static void busid(char *o)
{
	const char *h = "0123456789abcdef";
	memcpy(o, "0000:00:00.0", 13);
	o[5] = h[drm_pci_bus >> 4]; o[6] = h[drm_pci_bus & 15];
	o[8] = h[drm_pci_dev >> 4]; o[9] = h[drm_pci_dev & 15];
	o[11] = (char)('0' + drm_pci_fn);
}

/* The bus id appears once a client has asked for interface 1.1+ through
 * SET_VERSION, as on Linux: libdrm's drmOpenByName takes an empty one to mean
 * "this is the device", drmOpenByBusid sets the version and compares. */
static int busid_set;

static int ioc_get_unique(drm_filep_t *fp, void *d)
{
	(void)fp;
	struct drm_unique *u = d;
	char b[24] = "";
	if (busid_set) {
		if (drm_pci_found) { memcpy(b, "pci:", 4); busid(b + 4); }
		else strcpy(b, "simplefb");
	}
	return put_str(u->unique, &u->unique_len, b);
}

struct drm_set_version { int32_t di_major, di_minor, dd_major, dd_minor; };

static int ioc_set_version(drm_filep_t *fp, void *d)
{
	(void)fp;
	struct drm_set_version *v = d;
	int dd_minor = drm_display() ? (int)drm_display()->version_minor : 0;
	int e = 0;
	if (v->di_major != -1) {
		if (v->di_major != 1 || v->di_minor < 0 || v->di_minor > 4) e = -EINVAL_;
		else if (v->di_minor >= 1) busid_set = 1;
	}
	if (!e && v->dd_major != -1 && (v->dd_major != 1 || v->dd_minor < 0 || v->dd_minor > dd_minor)) e = -EINVAL_;
	v->di_major = 1; v->di_minor = 4;
	v->dd_major = 1; v->dd_minor = dd_minor;
	return e;
}

static int ioc_get_magic(drm_filep_t *fp, void *d)
{
	struct drm_auth *a = d;
	if (!fp->magic) fp->magic = drm_next_magic++;
	a->magic = fp->magic;
	return 0;
}

static int ioc_auth_magic(drm_filep_t *fp, void *d)
{
	struct drm_auth *a = d;
	if (fp != drm_master) return -EACCES_;
	return a->magic ? 0 : -EINVAL_;          /* libdrm's drmIsMaster asks with 0 */
}

static int ioc_set_master(drm_filep_t *fp, void *d)
{
	(void)d;
	if (drm_master && drm_master != fp) return -EBUSY_;
	drm_master = fp;
	return 0;
}

static int ioc_drop_master(drm_filep_t *fp, void *d)
{
	(void)d;
	if (drm_master != fp) return -EINVAL_;
	drm_master = NULL;
	return 0;
}

static int ioc_get_cap(drm_filep_t *fp, void *d)
{
	(void)fp;
	struct drm_get_cap *c = d;
	switch (c->capability) {
	case DRM_CAP_DUMB_BUFFER:          c->value = 1; break;
	case DRM_CAP_VBLANK_HIGH_CRTC:     c->value = 1; break;
	case DRM_CAP_DUMB_PREFERRED_DEPTH: c->value = 24; break;
	case DRM_CAP_DUMB_PREFER_SHADOW:   c->value = 0; break;   /* dumb maps are cached RAM */
	case DRM_CAP_PRIME:                c->value = DRM_PRIME_CAP_IMPORT | DRM_PRIME_CAP_EXPORT; break;
	case DRM_CAP_TIMESTAMP_MONOTONIC:  c->value = 1; break;
	case DRM_CAP_ASYNC_PAGE_FLIP:      c->value = 0; break;
	case DRM_CAP_CURSOR_WIDTH:         c->value = 64; break;
	case DRM_CAP_CURSOR_HEIGHT:        c->value = 64; break;
	case DRM_CAP_ADDFB2_MODIFIERS:     c->value = 1; break;
	case DRM_CAP_PAGE_FLIP_TARGET:     c->value = 0; break;
	case DRM_CAP_CRTC_IN_VBLANK_EVENT: c->value = 1; break;
	case DRM_CAP_SYNCOBJ:              c->value = 1; break;
	case DRM_CAP_SYNCOBJ_TIMELINE:     c->value = 1; break;
	case DRM_CAP_ATOMIC_ASYNC_PAGE_FLIP: c->value = 0; break;
	default: return -EINVAL_;
	}
	return 0;
}

static int ioc_set_client_cap(drm_filep_t *fp, void *d)
{
	struct drm_set_client_cap *c = d;
	if (c->value > 1) return -EINVAL_;
	switch (c->capability) {
	case DRM_CLIENT_CAP_STEREO_3D:        fp->stereo = (int)c->value; return 0;
	case DRM_CLIENT_CAP_UNIVERSAL_PLANES: fp->universal_planes = (int)c->value; return 0;
	case DRM_CLIENT_CAP_ASPECT_RATIO:     fp->aspect = (int)c->value; return 0;
	case DRM_CLIENT_CAP_ATOMIC:           return -EOPNOTSUPP_;   /* legacy KMS only, for now */
	case DRM_CLIENT_CAP_WRITEBACK_CONNECTORS:
	case DRM_CLIENT_CAP_CURSOR_PLANE_HOTSPOT: return -EINVAL_;
	}
	return -EINVAL_;
}

static int ioc_gem_close(drm_filep_t *fp, void *d)
{
	struct drm_gem_close *c = d;
	return gem_handle_delete(fp, c->handle);
}

static int ioc_gem_flink(drm_filep_t *fp, void *d)
{
	struct drm_gem_flink *a = d;
	drm_gem_t *o = drm_gem_lookup(fp, a->handle);
	if (!o) return -ENOENT_;
	if (!o->name) o->name = gem_next_name++;
	a->name = o->name;
	return 0;
}

static int ioc_gem_open(drm_filep_t *fp, void *d)
{
	struct drm_gem_open *a = d;
	if (!a->name) return -ENOENT_;
	for (drm_gem_t *o = gem_all; o; o = o->next)
		if (o->name == a->name) {
			int e = drm_gem_handle_new(fp, o, &a->handle);
			if (e) return e;
			a->size = o->size;
			return 0;
		}
	return -ENOENT_;
}

static int ioc_create_dumb(drm_filep_t *fp, void *d)
{
	struct drm_mode_create_dumb *c = d;
	if (c->flags || !c->width || !c->height || !c->bpp) return -EINVAL_;
	if (c->width > 16384 || c->height > 16384 || c->bpp > 64) return -EINVAL_;
	uint32_t cpp = (c->bpp + 7) / 8;
	uint64_t pitch = ((uint64_t)c->width * cpp + 63) & ~63ULL;   /* scanout wants 64-byte strides */
	uint64_t size = (pitch * c->height + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
	drm_gem_t *o = drm_gem_alloc(size);
	if (!o) return -ENOMEM_;
	uint32_t h;
	int e = drm_gem_handle_new(fp, o, &h);
	drm_gem_put(o);                          /* the handle holds it now */
	if (e) return e;
	c->handle = h;
	c->pitch  = (uint32_t)pitch;
	c->size   = size;
	return 0;
}

uint64_t drm_gem_mmap_offset(drm_gem_t *o)
{
	if (!o->mmap_off) { o->mmap_off = gem_next_off; gem_next_off += o->size + PAGE_SIZE; }
	return o->mmap_off;
}

static int ioc_map_dumb(drm_filep_t *fp, void *d)
{
	struct drm_mode_map_dumb *m = d;
	drm_gem_t *o = drm_gem_lookup(fp, m->handle);
	if (!o) return -ENOENT_;
	m->offset = drm_gem_mmap_offset(o);
	return 0;
}

static int ioc_destroy_dumb(drm_filep_t *fp, void *d)
{
	struct drm_mode_destroy_dumb *m = d;
	return gem_handle_delete(fp, m->handle);
}

static int ioc_prime_handle_to_fd(drm_filep_t *fp, void *d)
{
	struct drm_prime_handle *a = d;
	if (a->flags & ~(uint32_t)(DRM_CLOEXEC | DRM_RDWR)) return -EINVAL_;
	drm_gem_t *o = drm_gem_lookup(fp, a->handle);
	if (!o) return -ENOENT_;
	drm_gem_get(o);
	int fd = anon_fd(&dmabuf_fops, o, 0xdb000000ULL + (uint64_t)(uintptr_t)o / 64, (int)a->flags);
	if (fd < 0) { drm_gem_put(o); return fd; }
	a->fd = fd;
	return 0;
}

/* runs without drm_lock: the fput at its end may be the last and close a dma-buf */
static int ioc_prime_fd_to_handle(drm_filep_t *fp, void *d)
{
	struct drm_prime_handle *a = d;
	vfs_file_t *f = fget(task_current()->fds, a->fd);
	if (!f) return -EBADF_;
	int e = -EINVAL_;
	if (mutex_lock_killable(&drm_lock)) { e = -EINTR_; goto out; }
	if (f->inode && f->inode->fops == &dmabuf_fops && f->inode->priv) {
		drm_gem_t *o = f->inode->priv;
		uint32_t h = gem_handle_of(fp, o);
		e = h ? 0 : drm_gem_handle_new(fp, o, &h);
		if (!e) a->handle = h;
	}
	mutex_unlock(&drm_lock);
out:
	fput(f);
	return e;
}

/* ── syncobj ─────────────────────────────────────────────────── */
/* No engine runs work yet, so a fence is either there and signaled or not there.
 * Binary objects keep "signaled", timelines the highest signaled point. */

static wait_queue_t syncobj_wq = WAIT_QUEUE_INIT;

static drm_syncobj_t *syncobj_lookup(drm_filep_t *fp, uint32_t h)
{
	if (!h || h > fp->nsyncobjs) return NULL;
	return fp->syncobjs[h - 1];
}

/* for drivers: a file's syncobj, and signaling one (binary, or a timeline point) */
drm_syncobj_t *drm_syncobj_find(drm_filep_t *fp, uint32_t h) { return syncobj_lookup(fp, h); }
void drm_syncobj_signal(drm_syncobj_t *s, uint64_t point)
{
	if (point > s->point) s->point = point;
	s->signaled = 1;
	wake_up(&syncobj_wq);
}

static int syncobj_handle_new(drm_filep_t *fp, drm_syncobj_t *s, uint32_t *h)
{
	for (;;) {
		for (uint32_t i = 0; i < fp->nsyncobjs; i++)
			if (!fp->syncobjs[i]) { fp->syncobjs[i] = s; s->refcount++; *h = i + 1; return 0; }
		int e = slot_grow((void ***)&fp->syncobjs, &fp->nsyncobjs);
		if (e) return e;
	}
}

static int ioc_syncobj_create(drm_filep_t *fp, void *d)
{
	struct drm_syncobj_create *c = d;
	if (c->flags & ~(uint32_t)DRM_SYNCOBJ_CREATE_SIGNALED) return -EINVAL_;
	drm_syncobj_t *s = kmalloc(sizeof(*s));
	if (!s) return -ENOMEM_;
	memset(s, 0, sizeof(*s));
	s->signaled = (c->flags & DRM_SYNCOBJ_CREATE_SIGNALED) != 0;
	int e = syncobj_handle_new(fp, s, &c->handle);
	if (e) { kfree(s); return e; }
	return 0;
}

static int ioc_syncobj_destroy(drm_filep_t *fp, void *d)
{
	struct drm_syncobj_destroy *c = d;
	drm_syncobj_t *s = syncobj_lookup(fp, c->handle);
	if (!s || c->pad) return -EINVAL_;
	fp->syncobjs[c->handle - 1] = NULL;
	syncobj_put(s);
	return 0;
}

static int syncfile_poll(vfs_file_t *f) { (void)f; return 0x0001; }

static int syncfile_ioctl(vfs_file_t *f, uint32_t cmd, uint64_t arg)
{
	(void)f;
	if (cmd == SYNC_IOC_FILE_INFO) {
		struct { char name[32]; int32_t status; uint32_t flags, num_fences, pad; uint64_t info; } i;
		if (copy_from_user(&i, (void *)(uintptr_t)arg, sizeof(i))) return -EFAULT_;
		memset(i.name, 0, sizeof(i.name));
		memcpy(i.name, "ranix", 6);
		i.status = 1;                        /* signaled */
		i.num_fences = 0;
		return copy_to_user((void *)(uintptr_t)arg, &i, sizeof(i)) ? -EFAULT_ : 0;
	}
	if (cmd == SYNC_IOC_MERGE) {
		struct { char name[32]; int32_t fd2, fence; uint32_t flags, pad; } m;
		if (copy_from_user(&m, (void *)(uintptr_t)arg, sizeof(m))) return -EFAULT_;
		int fd = syncfile_new(DRM_CLOEXEC);
		if (fd < 0) return fd;
		m.fence = fd;
		return copy_to_user((void *)(uintptr_t)arg, &m, sizeof(m)) ? -EFAULT_ : 0;
	}
	return -ENOTTY_;
}

static int syncfile_close(vfs_file_t *f) { f->inode->priv = NULL; return 0; }

static vfs_fops_t syncfile_fops = {
	.poll  = syncfile_poll,
	.ioctl = syncfile_ioctl,
	.close = syncfile_close,
};

static uint64_t syncfile_ino = 0x5f000000ULL;
static int syncfile_new(int flags) { return anon_fd(&syncfile_fops, NULL, syncfile_ino++, flags); }
int drm_syncfile_new(void) { return syncfile_new(DRM_CLOEXEC); }
int drm_is_syncfile(vfs_file_t *f) { return f && f->inode && f->inode->fops == &syncfile_fops; }

static int syncobjfd_close(vfs_file_t *f)
{
	mutex_lock(&drm_lock);
	syncobj_put(f->inode->priv);
	f->inode->priv = NULL;
	mutex_unlock(&drm_lock);
	return 0;
}

static vfs_fops_t syncobj_fops = { .close = syncobjfd_close };

static int ioc_syncobj_handle_to_fd(drm_filep_t *fp, void *d)
{
	struct drm_syncobj_handle *a = d;
	if (a->pad || (a->flags & ~(uint32_t)DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_EXPORT_SYNC_FILE)) return -EINVAL_;
	drm_syncobj_t *s = syncobj_lookup(fp, a->handle);
	if (!s) return -ENOENT_;
	int fd;
	if (a->flags & DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_EXPORT_SYNC_FILE) {
		if (!s->signaled) return -EINVAL_;   /* no fence to export */
		fd = syncfile_new(DRM_CLOEXEC);
	} else {
		s->refcount++;
		fd = anon_fd(&syncobj_fops, s, 0x5e000000ULL + (uint64_t)(uintptr_t)s / 64, DRM_CLOEXEC | DRM_RDWR);
		if (fd < 0) s->refcount--;
	}
	if (fd < 0) return fd;
	a->fd = fd;
	return 0;
}

/* without drm_lock, like the PRIME import */
static int ioc_syncobj_fd_to_handle(drm_filep_t *fp, void *d)
{
	struct drm_syncobj_handle *a = d;
	if (a->pad || (a->flags & ~(uint32_t)DRM_SYNCOBJ_FD_TO_HANDLE_FLAGS_IMPORT_SYNC_FILE)) return -EINVAL_;
	vfs_file_t *f = fget(task_current()->fds, a->fd);
	if (!f) return -EINVAL_;
	int e = -EINVAL_;
	if (mutex_lock_killable(&drm_lock)) { e = -EINTR_; goto out; }
	if (a->flags & DRM_SYNCOBJ_FD_TO_HANDLE_FLAGS_IMPORT_SYNC_FILE) {
		drm_syncobj_t *s = syncobj_lookup(fp, a->handle);
		if (s && f->inode->fops == &syncfile_fops) { s->signaled = 1; e = 0; }
		else if (!s) e = -ENOENT_;
	} else if (f->inode->fops == &syncobj_fops && f->inode->priv) {
		e = syncobj_handle_new(fp, f->inode->priv, &a->handle);
	}
	mutex_unlock(&drm_lock);
out:
	fput(f);
	if (!e) wake_up(&syncobj_wq);
	return e;
}

static int syncobj_array(drm_filep_t *fp, uint64_t uh, uint32_t n, drm_syncobj_t **out)
{
	if (!n || n > SYNCOBJ_MAX) return -EINVAL_;
	for (uint32_t i = 0; i < n; i++) {
		uint32_t h;
		if (copy_from_user(&h, (void *)(uintptr_t)(uh + i * 4ULL), 4)) return -EFAULT_;
		out[i] = syncobj_lookup(fp, h);
		if (!out[i]) return -ENOENT_;
	}
	return 0;
}

static int ioc_syncobj_reset(drm_filep_t *fp, void *d)
{
	struct drm_syncobj_array *a = d;
	if (a->pad || a->count_handles > SYNCOBJ_MAX) return -EINVAL_;
	drm_syncobj_t *s[SYNCOBJ_MAX];
	int e = syncobj_array(fp, a->handles, a->count_handles, s);
	if (e) return e;
	for (uint32_t i = 0; i < a->count_handles; i++) s[i]->signaled = 0;
	return 0;
}

static int ioc_syncobj_signal(drm_filep_t *fp, void *d)
{
	struct drm_syncobj_array *a = d;
	if (a->pad || a->count_handles > SYNCOBJ_MAX) return -EINVAL_;
	drm_syncobj_t *s[SYNCOBJ_MAX];
	int e = syncobj_array(fp, a->handles, a->count_handles, s);
	if (e) return e;
	for (uint32_t i = 0; i < a->count_handles; i++) s[i]->signaled = 1;
	wake_up(&syncobj_wq);
	return 0;
}

static int ioc_syncobj_timeline_signal(drm_filep_t *fp, void *d)
{
	struct drm_syncobj_timeline_array *a = d;
	if (a->flags || a->count_handles > SYNCOBJ_MAX) return -EINVAL_;
	drm_syncobj_t *s[SYNCOBJ_MAX];
	int e = syncobj_array(fp, a->handles, a->count_handles, s);
	if (e) return e;
	for (uint32_t i = 0; i < a->count_handles; i++) {
		uint64_t pt;
		if (copy_from_user(&pt, (void *)(uintptr_t)(a->points + i * 8ULL), 8)) return -EFAULT_;
		if (pt > s[i]->point) s[i]->point = pt;
		s[i]->signaled = 1;
	}
	wake_up(&syncobj_wq);
	return 0;
}

static int ioc_syncobj_query(drm_filep_t *fp, void *d)
{
	struct drm_syncobj_timeline_array *a = d;
	if ((a->flags & ~(uint32_t)DRM_SYNCOBJ_QUERY_FLAGS_LAST_SUBMITTED) || a->count_handles > SYNCOBJ_MAX) return -EINVAL_;
	drm_syncobj_t *s[SYNCOBJ_MAX];
	int e = syncobj_array(fp, a->handles, a->count_handles, s);
	if (e) return e;
	for (uint32_t i = 0; i < a->count_handles; i++)
		if (copy_to_user((void *)(uintptr_t)(a->points + i * 8ULL), &s[i]->point, 8)) return -EFAULT_;
	return 0;
}

static int syncobj_done(drm_syncobj_t *s, uint64_t pt)
{
	return pt ? s->point >= pt : s->signaled;
}

static int ioc_syncobj_transfer(drm_filep_t *fp, void *d)
{
	struct drm_syncobj_transfer *t = d;
	if (t->pad) return -EINVAL_;
	drm_syncobj_t *src = syncobj_lookup(fp, t->src_handle), *dst = syncobj_lookup(fp, t->dst_handle);
	if (!src || !dst) return -ENOENT_;
	if (!syncobj_done(src, t->src_point)) return -EINVAL_;   /* nothing pending can exist */
	if (t->dst_point) { if (t->dst_point > dst->point) dst->point = t->dst_point; }
	dst->signaled = 1;
	wake_up(&syncobj_wq);
	return 0;
}

/* WAIT and TIMELINE_WAIT, without drm_lock while sleeping */
static int syncobj_wait_common(drm_filep_t *fp, uint64_t uh, uint64_t upts, uint32_t n,
                               int64_t timeout, uint32_t flags, uint32_t *first)
{
	if (!n || n > SYNCOBJ_MAX) return -EINVAL_;
	if (flags & ~0xfu) return -EINVAL_;
	drm_syncobj_t **s = kmalloc(n * (sizeof(drm_syncobj_t *) + sizeof(uint64_t)));
	if (!s) return -ENOMEM_;
	uint64_t *pts = (uint64_t *)(s + n);
	if (mutex_lock_killable(&drm_lock)) { kfree(s); return -EINTR_; }
	int e = syncobj_array(fp, uh, n, s);
	for (uint32_t i = 0; !e && i < n; i++) {
		pts[i] = 0;
		if (upts && copy_from_user(&pts[i], (void *)(uintptr_t)(upts + i * 8ULL), 8)) e = -EFAULT_;
	}
	if (!e) for (uint32_t i = 0; i < n; i++) s[i]->refcount++;
	mutex_unlock(&drm_lock);
	if (e) { kfree(s); return e; }
	int all = (flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL) != 0;
	int submit = (flags & (DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT | DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE)) != 0;
	int r = -ETIME_;
	/* a binary object without a fence fails at once unless the caller waits for submit */
	if (!submit) for (uint32_t i = 0; i < n; i++)
		if (!syncobj_done(s[i], pts[i])) { r = -EINVAL_; goto out; }
	for (;;) {
		uint32_t done = 0, fsig = n;
		for (uint32_t i = 0; i < n; i++)
			if (syncobj_done(s[i], pts[i])) { done++; if (fsig == n) fsig = i; }
		if (all ? done == n : done > 0) { *first = fsig == n ? 0 : fsig; r = 0; break; }
		if (timeout <= 0 || (uint64_t)timeout <= drm_now_ns()) { r = -ETIME_; break; }
		task_sleep_until(kernel_ticks + 1);
		if (task_has_actionable_signal()) { r = -EINTR_; break; }
	}
out:
	mutex_lock(&drm_lock);
	for (uint32_t i = 0; i < n; i++) syncobj_put(s[i]);
	mutex_unlock(&drm_lock);
	kfree(s);
	return r;
}

static int ioc_syncobj_wait(drm_filep_t *fp, void *d)
{
	struct drm_syncobj_wait *w = d;
	return syncobj_wait_common(fp, w->handles, 0, w->count_handles, w->timeout_nsec, w->flags, &w->first_signaled);
}

static int ioc_syncobj_timeline_wait(drm_filep_t *fp, void *d)
{
	struct drm_syncobj_timeline_wait *w = d;
	return syncobj_wait_common(fp, w->handles, w->points, w->count_handles, w->timeout_nsec, w->flags, &w->first_signaled);
}

static int ioc_atomic(drm_filep_t *fp, void *d) { (void)fp; (void)d; return -EOPNOTSUPP_; }

/* ── dispatch ────────────────────────────────────────────────── */

const drm_driver_t *drm_drv;   /* the hardware driver's ioctls (i915), or none */

#define D(nr, st, fl, f) [nr] = { sizeof(st), fl, f }
static const drm_ioctl_desc_t drm_ioctls[256] = {
	D(DRM_NR_VERSION,           struct drm_version,        F_RENDER, ioc_version),
	D(DRM_NR_GET_UNIQUE,        struct drm_unique,         0, ioc_get_unique),
	D(DRM_NR_GET_MAGIC,         struct drm_auth,           0, ioc_get_magic),
	D(DRM_NR_SET_VERSION,       struct drm_set_version,    0, ioc_set_version),
	D(DRM_NR_AUTH_MAGIC,        struct drm_auth,           0, ioc_auth_magic),
	D(DRM_NR_SET_MASTER,        uint32_t,                  0, ioc_set_master),
	D(DRM_NR_DROP_MASTER,       uint32_t,                  0, ioc_drop_master),
	D(DRM_NR_GEM_CLOSE,         struct drm_gem_close,      F_RENDER, ioc_gem_close),
	D(DRM_NR_GEM_FLINK,         struct drm_gem_flink,      0, ioc_gem_flink),
	D(DRM_NR_GEM_OPEN,          struct drm_gem_open,       0, ioc_gem_open),
	D(DRM_NR_GET_CAP,           struct drm_get_cap,        F_RENDER, ioc_get_cap),
	D(DRM_NR_SET_CLIENT_CAP,    struct drm_set_client_cap, 0, ioc_set_client_cap),
	D(DRM_NR_PRIME_HANDLE_TO_FD, struct drm_prime_handle,  F_RENDER, ioc_prime_handle_to_fd),
	D(DRM_NR_PRIME_FD_TO_HANDLE, struct drm_prime_handle,  F_RENDER | F_NOLOCK, ioc_prime_fd_to_handle),
	D(DRM_NR_WAIT_VBLANK,       union drm_wait_vblank,     F_CARD | F_NOLOCK, drm_ioctl_wait_vblank),
	D(DRM_NR_CRTC_GET_SEQUENCE, struct drm_crtc_get_sequence, F_CARD, drm_ioctl_crtc_get_sequence),
	D(DRM_NR_CRTC_QUEUE_SEQUENCE, struct drm_crtc_queue_sequence, F_CARD, drm_ioctl_crtc_queue_sequence),
	D(DRM_NR_MODE_GETRESOURCES, struct drm_mode_card_res,  F_CARD, drm_ioctl_getresources),
	D(DRM_NR_MODE_GETCRTC,      struct drm_mode_crtc,      F_CARD, drm_ioctl_getcrtc),
	D(DRM_NR_MODE_SETCRTC,      struct drm_mode_crtc,      F_CARD | F_MASTER, drm_ioctl_setcrtc),
	D(DRM_NR_MODE_CURSOR,       struct drm_mode_cursor,    F_CARD | F_MASTER, drm_ioctl_cursor),
	D(DRM_NR_MODE_CURSOR2,      struct drm_mode_cursor2,   F_CARD | F_MASTER, drm_ioctl_cursor2),
	D(DRM_NR_MODE_GETGAMMA,     struct drm_mode_crtc_lut,  F_CARD, drm_ioctl_getgamma),
	D(DRM_NR_MODE_SETGAMMA,     struct drm_mode_crtc_lut,  F_CARD | F_MASTER, drm_ioctl_setgamma),
	D(DRM_NR_MODE_GETENCODER,   struct drm_mode_get_encoder, F_CARD, drm_ioctl_getencoder),
	D(DRM_NR_MODE_GETCONNECTOR, struct drm_mode_get_connector, F_CARD, drm_ioctl_getconnector),
	D(DRM_NR_MODE_GETPROPERTY,  struct drm_mode_get_property, F_CARD, drm_ioctl_getproperty),
	D(DRM_NR_MODE_SETPROPERTY,  struct drm_mode_connector_set_property, F_CARD | F_MASTER, drm_ioctl_setproperty),
	D(DRM_NR_MODE_GETPROPBLOB,  struct drm_mode_get_blob,  F_CARD, drm_ioctl_getpropblob),
	D(DRM_NR_MODE_GETFB,        struct drm_mode_fb_cmd,    F_CARD, drm_ioctl_getfb),
	D(DRM_NR_MODE_GETFB2,       struct drm_mode_fb_cmd2,   F_CARD, drm_ioctl_getfb2),
	D(DRM_NR_MODE_ADDFB,        struct drm_mode_fb_cmd,    F_CARD, drm_ioctl_addfb),
	D(DRM_NR_MODE_ADDFB2,       struct drm_mode_fb_cmd2,   F_CARD, drm_ioctl_addfb2),
	D(DRM_NR_MODE_RMFB,         uint32_t,                  F_CARD, drm_ioctl_rmfb),
	D(DRM_NR_MODE_CLOSEFB,      struct drm_mode_closefb,   F_CARD, drm_ioctl_closefb),
	D(DRM_NR_MODE_PAGE_FLIP,    struct drm_mode_crtc_page_flip, F_CARD | F_MASTER, drm_ioctl_page_flip),
	D(DRM_NR_MODE_DIRTYFB,      struct drm_mode_fb_dirty_cmd, F_CARD, drm_ioctl_dirtyfb),
	D(DRM_NR_MODE_CREATE_DUMB,  struct drm_mode_create_dumb, F_CARD, ioc_create_dumb),
	D(DRM_NR_MODE_MAP_DUMB,     struct drm_mode_map_dumb,  F_CARD, ioc_map_dumb),
	D(DRM_NR_MODE_DESTROY_DUMB, struct drm_mode_destroy_dumb, F_CARD, ioc_destroy_dumb),
	D(DRM_NR_MODE_GETPLANERESOURCES, struct drm_mode_get_plane_res, F_CARD, drm_ioctl_getplaneresources),
	D(DRM_NR_MODE_GETPLANE,     struct drm_mode_get_plane, F_CARD, drm_ioctl_getplane),
	D(DRM_NR_MODE_SETPLANE,     struct drm_mode_set_plane, F_CARD | F_MASTER, drm_ioctl_setplane),
	D(DRM_NR_MODE_OBJ_GETPROPERTIES, struct drm_mode_obj_get_properties, F_CARD, drm_ioctl_obj_getproperties),
	D(DRM_NR_MODE_OBJ_SETPROPERTY, struct drm_mode_obj_set_property, F_CARD | F_MASTER, drm_ioctl_obj_setproperty),
	D(DRM_NR_MODE_ATOMIC,       uint64_t,                  F_CARD, ioc_atomic),
	D(DRM_NR_MODE_CREATEPROPBLOB, struct drm_mode_create_blob, F_CARD, drm_ioctl_createpropblob),
	D(DRM_NR_MODE_DESTROYPROPBLOB, struct drm_mode_destroy_blob, F_CARD, drm_ioctl_destroypropblob),
	D(DRM_NR_SYNCOBJ_CREATE,    struct drm_syncobj_create,  F_RENDER, ioc_syncobj_create),
	D(DRM_NR_SYNCOBJ_DESTROY,   struct drm_syncobj_destroy, F_RENDER, ioc_syncobj_destroy),
	D(DRM_NR_SYNCOBJ_HANDLE_TO_FD, struct drm_syncobj_handle, F_RENDER, ioc_syncobj_handle_to_fd),
	D(DRM_NR_SYNCOBJ_FD_TO_HANDLE, struct drm_syncobj_handle, F_RENDER | F_NOLOCK, ioc_syncobj_fd_to_handle),
	D(DRM_NR_SYNCOBJ_WAIT,      struct drm_syncobj_wait,    F_RENDER | F_NOLOCK, ioc_syncobj_wait),
	D(DRM_NR_SYNCOBJ_RESET,     struct drm_syncobj_array,   F_RENDER, ioc_syncobj_reset),
	D(DRM_NR_SYNCOBJ_SIGNAL,    struct drm_syncobj_array,   F_RENDER, ioc_syncobj_signal),
	D(DRM_NR_SYNCOBJ_TIMELINE_WAIT, struct drm_syncobj_timeline_wait, F_RENDER | F_NOLOCK, ioc_syncobj_timeline_wait),
	D(DRM_NR_SYNCOBJ_QUERY,     struct drm_syncobj_timeline_array, F_RENDER, ioc_syncobj_query),
	D(DRM_NR_SYNCOBJ_TRANSFER,  struct drm_syncobj_transfer, F_RENDER, ioc_syncobj_transfer),
	D(DRM_NR_SYNCOBJ_TIMELINE_SIGNAL, struct drm_syncobj_timeline_array, F_RENDER, ioc_syncobj_timeline_signal),
};

static int drm_ioctl(vfs_file_t *f, uint32_t cmd, uint64_t arg)
{
	drm_filep_t *fp = f->priv;
	if (!fp) return -EBADF_;
	if (DRM_IOC_TYPE(cmd) != DRM_IOCTL_BASE) return -ENOTTY_;
	uint32_t nr = DRM_IOC_NR(cmd);
	const drm_ioctl_desc_t *dsc;
	if (nr >= DRM_COMMAND_BASE && nr < DRM_COMMAND_END) {
		if (!drm_drv || nr - DRM_COMMAND_BASE >= drm_drv->nioctls) return -EINVAL_;
		dsc = &drm_drv->ioctls[nr - DRM_COMMAND_BASE];
	} else dsc = &drm_ioctls[nr];
	if (!dsc->fn) return -EINVAL_;
	if (fp->render && !(dsc->flags & F_RENDER)) return -EACCES_;

	uint8_t kbuf[256] __attribute__((aligned(8)));
	uint32_t usize = DRM_IOC_SIZE(cmd), dir = DRM_IOC_DIR(cmd);
	uint32_t n = usize < dsc->size ? usize : dsc->size;
	if (dsc->size > sizeof(kbuf)) return -EINVAL_;
	memset(kbuf, 0, sizeof(kbuf));           /* an older, shorter struct reads as zeros past its end */
	if ((dir & DRM_IOC_WRITE) && n && copy_from_user(kbuf, (void *)(uintptr_t)arg, n)) return -EFAULT_;

	int r;
	if (dsc->flags & F_NOLOCK) {
		r = dsc->fn(fp, kbuf);
	} else {
		if (mutex_lock_killable(&drm_lock)) return -EINTR_;
		if ((dsc->flags & F_MASTER) && drm_master != fp) {
			/* nobody holds the card: whoever modesets now becomes master, as on Linux
			 * when the previous master has closed */
			if (!drm_master) drm_master = fp;
		}
		r = ((dsc->flags & F_MASTER) && drm_master != fp) ? -EACCES_ : dsc->fn(fp, kbuf);
		mutex_unlock(&drm_lock);
	}
	if ((dir & DRM_IOC_READ) && n && copy_to_user((void *)(uintptr_t)arg, kbuf, n)) return -EFAULT_;
	return r;
}

static vfs_fops_t drm_fops = {
	.open      = drm_open,
	.close     = drm_close,
	.read      = drm_read,
	.poll      = drm_poll,
	.ioctl     = drm_ioctl,
	.mmap_page = drm_mmap_page,
};

/* ── init ────────────────────────────────────────────────────── */

void drm_init(void)
{
	uint8_t b, dv, fn;
	if (pci_find_class(0x03, 0x00, 0x00, &b, &dv, &fn) == 0) {
		drm_pci_found = 1;
		drm_pci_bus = b; drm_pci_dev = dv; drm_pci_fn = fn;
	}
	if (drm_kms_init() != 0) return;          /* no display we can drive */
	const drm_display_t *hw = drm_display();
	if (hw) {
		strcpy(drm_driver_name, hw->name);
		if (hw->driver) drm_drv = hw->driver();   /* GEM/execbuffer ioctls; the GPU itself starts on first use */
	}
	drm_ready = 1;
	devfs_register_device_gid("dri/card0", S_IFCHR | 0660, DEVFS_GID_VIDEO, (DRM_MAJOR << 8) | 0, &drm_fops);
	devfs_register_device_gid("dri/renderD128", S_IFCHR | 0660, DEVFS_GID_VIDEO, (DRM_MAJOR << 8) | DRM_MINOR_RENDER, &drm_fops);
	tick_register(drm_kms_tick);
	kprintf("drm: %s on /dev/dri/card0 and renderD128\n", drm_driver_name);
}

int drm_is_ready(void) { return drm_ready; }
