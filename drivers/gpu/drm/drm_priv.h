/*
 * Ranix DRM: shared state of the core (drm_core.c), KMS (drm_kms.c) and the
 * /sys tree (drm_sysfs.c). One device: card0 (226:0) and renderD128 (226:128).
 */
#ifndef RANIX_DRM_PRIV_H
#define RANIX_DRM_PRIV_H

#include <uapi/types.h>
#include <kernel/vfs.h>
#include <kernel/sync.h>
#include <kernel/fb.h>
#include <kernel/time.h>
#include "drm_uapi.h"

#define DRM_MAJOR        226
#define DRM_MINOR_RENDER 128

#define EPERM_   1
#define E2BIG_   7
#define ENOENT_  2
#define EINTR_   4
#define EIO_     5
#define EBADF_   9
#define EAGAIN_  11
#define ENOMEM_  12
#define EACCES_  13
#define EFAULT_  14
#define EBUSY_   16
#define EEXIST_  17
#define ENODEV_  19
#define EINVAL_  22
#define ENOSPC_  28
#define ERANGE_  34
#define ENOTTY_  25
#define ETIME_   62
#define EOPNOTSUPP_ 95

/* A buffer: whole pages, not contiguous. Every handle, framebuffer, dma-buf fd
 * and the cursor hold one reference. User mappings hold their own page refs, so a
 * mapping outliving the object keeps its pages and nothing else. */
typedef struct drm_gem {
	uint32_t  refcount;     /* under drm_lock */
	uint32_t  npages;
	uint64_t  size;
	uint64_t *pages;        /* physical addresses */
	uint64_t  mmap_off;     /* fake offset on the card fd, 0 = none yet */
	uint32_t  name;         /* flink name, 0 = none */
	uint64_t  ggtt;         /* display (GGTT) address while bound, 0 = unbound */
	uint32_t  gpu_seqno;    /* last GPU request that used it (0 = never) */
	uint32_t  caching;      /* i915 SET_CACHING value, kept for GET_CACHING */
	uint32_t  ggtt_slots;
	struct drm_gem *next;   /* all objects: flink lookup */
} drm_gem_t;

typedef struct drm_syncobj {
	uint32_t refcount;
	int      signaled;      /* binary payload: a signaled fence, or none */
	uint64_t point;         /* timeline payload: highest signaled point */
} drm_syncobj_t;

struct drm_filep;

/* An event waiting for its vblank, then in the file's read queue. */
typedef struct drm_pending {
	struct drm_pending *next;
	struct drm_filep   *file;
	uint64_t target;        /* vblank sequence it fires at */
	int      kind;          /* DRM_EVENT_* : how to stamp it */
	uint32_t len;
	union {
		struct drm_event                base;
		struct drm_event_vblank         vbl;
		struct drm_event_crtc_sequence  seq;
	} ev;
} drm_pending_t;

typedef struct drm_filep {
	int             render;         /* opened through renderD128 */
	drm_gem_t     **handles;        /* handle h is handles[h-1] */
	uint32_t        nhandles;
	drm_syncobj_t **syncobjs;
	uint32_t        nsyncobjs;
	drm_pending_t  *evq, *evq_tail; /* under drm_ev_lock */
	uint32_t        ev_space;
	wait_queue_t    wq;
	int             universal_planes, atomic, stereo, aspect;
	uint32_t        magic;
	void           *driver_priv;    /* i915: contexts, address space */
} drm_filep_t;

typedef struct drm_fb {
	uint32_t   id;
	uint32_t   refcount;
	drm_filep_t *owner;
	drm_gem_t *obj;
	uint32_t   width, height, pitch, offset, format, cpp;
	struct drm_fb *next;
} drm_fb_t;

#define F_RENDER 1   /* allowed on the render node */
#define F_MASTER 2   /* only the master of the card */
#define F_NOLOCK 4   /* handler takes drm_lock itself */
#define F_CARD   8   /* card node only (KMS, dumb) */
#define DRM_COMMAND_BASE 0x40
#define DRM_COMMAND_END  0xA0

typedef int (*drm_ioctl_fn)(drm_filep_t *, void *);
typedef struct { uint16_t size; uint8_t flags; drm_ioctl_fn fn; } drm_ioctl_desc_t;

/* a hardware driver on top of the core (i915/) */
typedef struct {
	const drm_ioctl_desc_t *ioctls;            /* indexed by nr - DRM_COMMAND_BASE */
	uint32_t nioctls;
	void (*close)(drm_filep_t *fp);           /* under drm_lock, before handles are dropped */
	void (*handle_close)(drm_filep_t *fp, uint32_t handle, drm_gem_t *o);   /* under drm_lock */
} drm_driver_t;
extern const drm_driver_t *drm_drv;

extern mutex_t     drm_lock;
extern spinlock_t  drm_ev_lock;
extern drm_filep_t *drm_master;
extern char        drm_driver_name[16];

/* core */
drm_gem_t *drm_gem_lookup(drm_filep_t *fp, uint32_t handle);
void       drm_gem_get(drm_gem_t *o);
void       drm_gem_put(drm_gem_t *o);
int        drm_gem_handle_new(drm_filep_t *fp, drm_gem_t *o, uint32_t *handle);
drm_gem_t *drm_gem_alloc(uint64_t size);
void      *drm_gem_vaddr(drm_gem_t *o, uint64_t off, uint64_t *avail);
uint64_t   drm_gem_mmap_offset(drm_gem_t *o);
drm_syncobj_t *drm_syncobj_find(drm_filep_t *fp, uint32_t h);
void       drm_syncobj_signal(drm_syncobj_t *s, uint64_t point);
int        drm_syncfile_new(void);              /* a signaled sync_file fd */
int        drm_is_syncfile(vfs_file_t *f);
int        drm_event_space_take(drm_filep_t *fp, uint32_t len);
void       drm_event_deliver(drm_pending_t *p);    /* under drm_ev_lock */
void       drm_event_free_unqueued(drm_pending_t *p);
uint64_t   drm_now_ns(void);
void       drm_sleep_ms(uint32_t ms);          /* task context */
void       drm_cache_flush(void *p, uint64_t len);   /* write CPU cache lines back for a non-snooping reader */
void       drm_gem_flush(drm_gem_t *o, uint64_t off, uint64_t len);

/* The firmware's framebuffer, as the loader handed it over. */
typedef struct { uint32_t w, h, pitch; uint8_t *mem; uint64_t size; } drm_fw_fb_t;

/* A display back end (i915/intel_display.c). Without one, KMS copies client
 * buffers into the firmware framebuffer and keeps a 60 Hz software vblank. */
typedef struct {
	const char *name, *desc;                   /* "i915", for DRM_IOCTL_VERSION */
	const char *sysfs_driver;                  /* driver link under /sys/bus */
	uint32_t version_minor;
	uint32_t conn_type, enc_type;
	int  (*vblank)(uint32_t *count, uint64_t *since_ns);   /* 0: no hardware counter */
	void (*wait_vblank_start)(void);           /* bounded */
	int  (*flip)(drm_gem_t *o, uint32_t pitch);   /* 0: plane points at o from the next vblank */
	void (*flip_firmware)(void);               /* plane back on the firmware buffer */
	int  (*flip_landed)(void);                 /* the last flip is on screen */
	void (*release)(drm_gem_t *o);             /* object going away */
	void (*tick)(void);                        /* every 1 ms, interrupt context */
	int  (*debug)(char *o, int max);
	int  (*selftest)(void);
	const drm_driver_t *(*driver)(void);       /* GEM/execbuffer ioctls, or NULL */
} drm_display_t;
const drm_display_t *drm_display(void);         /* NULL: firmware framebuffer only */
const drm_display_t *intel_display_probe(const drm_fw_fb_t *fb, struct drm_mode_modeinfo *mode,
                                         uint64_t *frame_ns, int *can_flip);

/* KMS (drm_kms.c) */
int  drm_kms_init(void);
void drm_kms_file_close(drm_filep_t *fp);
void drm_kms_gem_release(drm_gem_t *o);
void drm_kms_tick(void);
int  drm_kms_mode_size(uint32_t *w, uint32_t *h);
int  drm_kms_connector_status(void);
uint32_t drm_kms_dpms(void);
int  drm_kms_hwflip(int set);       /* set < 0: query */
int  drm_kms_debug(char *o);        /* one line, < 400 bytes */
int  drm_kms_selftest(void);

int drm_ioctl_getresources(drm_filep_t *fp, void *d);
int drm_ioctl_getcrtc(drm_filep_t *fp, void *d);
int drm_ioctl_setcrtc(drm_filep_t *fp, void *d);
int drm_ioctl_cursor(drm_filep_t *fp, void *d);
int drm_ioctl_cursor2(drm_filep_t *fp, void *d);
int drm_ioctl_getgamma(drm_filep_t *fp, void *d);
int drm_ioctl_setgamma(drm_filep_t *fp, void *d);
int drm_ioctl_getencoder(drm_filep_t *fp, void *d);
int drm_ioctl_getconnector(drm_filep_t *fp, void *d);
int drm_ioctl_getproperty(drm_filep_t *fp, void *d);
int drm_ioctl_setproperty(drm_filep_t *fp, void *d);
int drm_ioctl_getpropblob(drm_filep_t *fp, void *d);
int drm_ioctl_createpropblob(drm_filep_t *fp, void *d);
int drm_ioctl_destroypropblob(drm_filep_t *fp, void *d);
int drm_ioctl_getfb(drm_filep_t *fp, void *d);
int drm_ioctl_getfb2(drm_filep_t *fp, void *d);
int drm_ioctl_addfb(drm_filep_t *fp, void *d);
int drm_ioctl_addfb2(drm_filep_t *fp, void *d);
int drm_ioctl_rmfb(drm_filep_t *fp, void *d);
int drm_ioctl_closefb(drm_filep_t *fp, void *d);
int drm_ioctl_page_flip(drm_filep_t *fp, void *d);
int drm_ioctl_dirtyfb(drm_filep_t *fp, void *d);
int drm_ioctl_getplaneresources(drm_filep_t *fp, void *d);
int drm_ioctl_getplane(drm_filep_t *fp, void *d);
int drm_ioctl_setplane(drm_filep_t *fp, void *d);
int drm_ioctl_obj_getproperties(drm_filep_t *fp, void *d);
int drm_ioctl_obj_setproperty(drm_filep_t *fp, void *d);
int drm_ioctl_wait_vblank(drm_filep_t *fp, void *d);       /* called without drm_lock */
int drm_ioctl_crtc_get_sequence(drm_filep_t *fp, void *d);
int drm_ioctl_crtc_queue_sequence(drm_filep_t *fp, void *d);

/* drm_sysfs_root ("dev" or "class/drm") is declared in kernel/vfs.h, the boot
 * framebuffer in kernel/fb.h, kernel_ticks and mono_ns in kernel/time.h */
int drm_is_ready(void);

/* PCI function of the display, for sysfs and the busid */
extern int     drm_pci_found;
extern uint8_t drm_pci_bus, drm_pci_dev, drm_pci_fn;

#endif
