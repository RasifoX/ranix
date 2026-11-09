/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/*
 * The DRM user ABI: ioctl numbers, structs and constants that libdrm, Mesa and
 * compositors compile against. Layouts match Linux include/uapi/drm/{drm,drm_mode}.h
 * byte for byte (they are the ABI); everything else in drivers/gpu/drm is Ranix's own.
 */
#ifndef RANIX_DRM_UAPI_H
#define RANIX_DRM_UAPI_H

#include <uapi/types.h>

/* _IOC as on x86_64/aarch64 Linux: nr 0-7, type 8-15, size 16-29, dir 30-31 */
#define DRM_IOC_NR(c)    ((c) & 0xff)
#define DRM_IOC_TYPE(c)  (((c) >> 8) & 0xff)
#define DRM_IOC_SIZE(c)  (((c) >> 16) & 0x3fff)
#define DRM_IOC_DIR(c)   (((c) >> 30) & 3)
#define DRM_IOC_WRITE    1u
#define DRM_IOC_READ     2u
#define DRM_IOCTL_BASE   'd'

/* core */
#define DRM_NR_VERSION            0x00
#define DRM_NR_GET_UNIQUE         0x01
#define DRM_NR_GET_MAGIC          0x02
#define DRM_NR_IRQ_BUSID          0x03
#define DRM_NR_GET_MAP            0x04
#define DRM_NR_GET_CLIENT         0x05
#define DRM_NR_GET_STATS          0x06
#define DRM_NR_SET_VERSION        0x07
#define DRM_NR_MODESET_CTL        0x08
#define DRM_NR_GEM_CLOSE          0x09
#define DRM_NR_GEM_FLINK          0x0a
#define DRM_NR_GEM_OPEN           0x0b
#define DRM_NR_GET_CAP            0x0c
#define DRM_NR_SET_CLIENT_CAP     0x0d
#define DRM_NR_AUTH_MAGIC         0x11
#define DRM_NR_SET_MASTER         0x1e
#define DRM_NR_DROP_MASTER        0x1f
#define DRM_NR_PRIME_HANDLE_TO_FD 0x2d
#define DRM_NR_PRIME_FD_TO_HANDLE 0x2e
#define DRM_NR_WAIT_VBLANK        0x3a
#define DRM_NR_CRTC_GET_SEQUENCE  0x3b
#define DRM_NR_CRTC_QUEUE_SEQUENCE 0x3c
/* KMS */
#define DRM_NR_MODE_GETRESOURCES  0xA0
#define DRM_NR_MODE_GETCRTC       0xA1
#define DRM_NR_MODE_SETCRTC       0xA2
#define DRM_NR_MODE_CURSOR        0xA3
#define DRM_NR_MODE_GETGAMMA      0xA4
#define DRM_NR_MODE_SETGAMMA      0xA5
#define DRM_NR_MODE_GETENCODER    0xA6
#define DRM_NR_MODE_GETCONNECTOR  0xA7
#define DRM_NR_MODE_GETPROPERTY   0xAA
#define DRM_NR_MODE_SETPROPERTY   0xAB
#define DRM_NR_MODE_GETPROPBLOB   0xAC
#define DRM_NR_MODE_GETFB         0xAD
#define DRM_NR_MODE_ADDFB         0xAE
#define DRM_NR_MODE_RMFB          0xAF
#define DRM_NR_MODE_PAGE_FLIP     0xB0
#define DRM_NR_MODE_DIRTYFB       0xB1
#define DRM_NR_MODE_CREATE_DUMB   0xB2
#define DRM_NR_MODE_MAP_DUMB      0xB3
#define DRM_NR_MODE_DESTROY_DUMB  0xB4
#define DRM_NR_MODE_GETPLANERESOURCES 0xB5
#define DRM_NR_MODE_GETPLANE      0xB6
#define DRM_NR_MODE_SETPLANE      0xB7
#define DRM_NR_MODE_ADDFB2        0xB8
#define DRM_NR_MODE_OBJ_GETPROPERTIES 0xB9
#define DRM_NR_MODE_OBJ_SETPROPERTY   0xBA
#define DRM_NR_MODE_CURSOR2       0xBB
#define DRM_NR_MODE_ATOMIC        0xBC
#define DRM_NR_MODE_CREATEPROPBLOB 0xBD
#define DRM_NR_MODE_DESTROYPROPBLOB 0xBE
#define DRM_NR_SYNCOBJ_CREATE     0xBF
#define DRM_NR_SYNCOBJ_DESTROY    0xC0
#define DRM_NR_SYNCOBJ_HANDLE_TO_FD 0xC1
#define DRM_NR_SYNCOBJ_FD_TO_HANDLE 0xC2
#define DRM_NR_SYNCOBJ_WAIT       0xC3
#define DRM_NR_SYNCOBJ_RESET      0xC4
#define DRM_NR_SYNCOBJ_SIGNAL     0xC5
#define DRM_NR_MODE_CREATE_LEASE  0xC6
#define DRM_NR_SYNCOBJ_TIMELINE_WAIT 0xCA
#define DRM_NR_SYNCOBJ_QUERY      0xCB
#define DRM_NR_SYNCOBJ_TRANSFER   0xCC
#define DRM_NR_SYNCOBJ_TIMELINE_SIGNAL 0xCD
#define DRM_NR_MODE_GETFB2        0xCE
#define DRM_NR_SYNCOBJ_EVENTFD    0xCF
#define DRM_NR_MODE_CLOSEFB       0xD0

/* capabilities */
#define DRM_CAP_DUMB_BUFFER          0x1
#define DRM_CAP_VBLANK_HIGH_CRTC     0x2
#define DRM_CAP_DUMB_PREFERRED_DEPTH 0x3
#define DRM_CAP_DUMB_PREFER_SHADOW   0x4
#define DRM_CAP_PRIME                0x5
#define  DRM_PRIME_CAP_IMPORT        0x1
#define  DRM_PRIME_CAP_EXPORT        0x2
#define DRM_CAP_TIMESTAMP_MONOTONIC  0x6
#define DRM_CAP_ASYNC_PAGE_FLIP      0x7
#define DRM_CAP_CURSOR_WIDTH         0x8
#define DRM_CAP_CURSOR_HEIGHT        0x9
#define DRM_CAP_ADDFB2_MODIFIERS     0x10
#define DRM_CAP_PAGE_FLIP_TARGET     0x11
#define DRM_CAP_CRTC_IN_VBLANK_EVENT 0x12
#define DRM_CAP_SYNCOBJ              0x13
#define DRM_CAP_SYNCOBJ_TIMELINE     0x14
#define DRM_CAP_ATOMIC_ASYNC_PAGE_FLIP 0x15

#define DRM_CLIENT_CAP_STEREO_3D            1
#define DRM_CLIENT_CAP_UNIVERSAL_PLANES     2
#define DRM_CLIENT_CAP_ATOMIC               3
#define DRM_CLIENT_CAP_ASPECT_RATIO         4
#define DRM_CLIENT_CAP_WRITEBACK_CONNECTORS 5
#define DRM_CLIENT_CAP_CURSOR_PLANE_HOTSPOT 6

struct drm_version {
	int32_t  version_major;
	int32_t  version_minor;
	int32_t  version_patchlevel;
	uint64_t name_len;
	uint64_t name;      /* char __user * */
	uint64_t date_len;
	uint64_t date;
	uint64_t desc_len;
	uint64_t desc;
};

struct drm_unique { uint64_t unique_len; uint64_t unique; };
struct drm_auth { uint32_t magic; };
struct drm_gem_close { uint32_t handle, pad; };
struct drm_gem_flink { uint32_t handle, name; };
struct drm_gem_open { uint32_t name, handle; uint64_t size; };
struct drm_get_cap { uint64_t capability, value; };
struct drm_set_client_cap { uint64_t capability, value; };
struct drm_prime_handle { uint32_t handle, flags; int32_t fd; };

#define DRM_RDWR    0x2       /* O_RDWR */
#define DRM_CLOEXEC 0x80000   /* O_CLOEXEC */

/* events, read(2) from the fd */
struct drm_event { uint32_t type, length; };
#define DRM_EVENT_VBLANK         0x01
#define DRM_EVENT_FLIP_COMPLETE  0x02
#define DRM_EVENT_CRTC_SEQUENCE  0x03
struct drm_event_vblank {
	struct drm_event base;
	uint64_t user_data;
	uint32_t tv_sec, tv_usec, sequence, crtc_id;
};
struct drm_event_crtc_sequence {
	struct drm_event base;
	uint64_t user_data;
	int64_t  time_ns;
	uint64_t sequence;
};

/* WAIT_VBLANK */
#define _DRM_VBLANK_ABSOLUTE      0x0
#define _DRM_VBLANK_RELATIVE      0x1
#define _DRM_VBLANK_HIGH_CRTC_MASK 0x0000003e
#define _DRM_VBLANK_HIGH_CRTC_SHIFT 1
#define _DRM_VBLANK_EVENT         0x4000000
#define _DRM_VBLANK_FLIP          0x8000000
#define _DRM_VBLANK_NEXTONMISS    0x10000000
#define _DRM_VBLANK_SECONDARY     0x20000000
#define _DRM_VBLANK_SIGNAL        0x40000000
#define _DRM_VBLANK_TYPES_MASK    (_DRM_VBLANK_ABSOLUTE | _DRM_VBLANK_RELATIVE)
union drm_wait_vblank {
	struct { uint32_t type, sequence; uint64_t signal; } request;
	struct { uint32_t type, sequence; int64_t tval_sec, tval_usec; } reply;
};
struct drm_crtc_get_sequence { uint32_t crtc_id, active; uint64_t sequence; int64_t sequence_ns; };
#define DRM_CRTC_SEQUENCE_RELATIVE     0x1
#define DRM_CRTC_SEQUENCE_NEXT_ON_MISS 0x2
struct drm_crtc_queue_sequence { uint32_t crtc_id, flags; uint64_t sequence, user_data; };

/* syncobj */
#define DRM_SYNCOBJ_CREATE_SIGNALED (1 << 0)
#define DRM_SYNCOBJ_FD_TO_HANDLE_FLAGS_IMPORT_SYNC_FILE (1 << 0)
#define DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_EXPORT_SYNC_FILE (1 << 0)
#define DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL        (1 << 0)
#define DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT (1 << 1)
#define DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE  (1 << 2)
#define DRM_SYNCOBJ_WAIT_FLAGS_WAIT_DEADLINE   (1 << 3)
#define DRM_SYNCOBJ_QUERY_FLAGS_LAST_SUBMITTED (1 << 0)
struct drm_syncobj_create { uint32_t handle, flags; };
struct drm_syncobj_destroy { uint32_t handle, pad; };
struct drm_syncobj_handle { uint32_t handle, flags; int32_t fd; uint32_t pad; };
struct drm_syncobj_wait {
	uint64_t handles; int64_t timeout_nsec;
	uint32_t count_handles, flags, first_signaled, pad;
	uint64_t deadline_nsec;
};
struct drm_syncobj_timeline_wait {
	uint64_t handles, points; int64_t timeout_nsec;
	uint32_t count_handles, flags, first_signaled, pad;
	uint64_t deadline_nsec;
};
struct drm_syncobj_array { uint64_t handles; uint32_t count_handles, pad; };
struct drm_syncobj_timeline_array { uint64_t handles, points; uint32_t count_handles, flags; };
struct drm_syncobj_transfer { uint32_t src_handle, dst_handle; uint64_t src_point, dst_point; uint32_t flags, pad; };

/* ── KMS ── */
#define DRM_DISPLAY_MODE_LEN 32
#define DRM_PROP_NAME_LEN    32

#define DRM_MODE_TYPE_PREFERRED (1 << 3)
#define DRM_MODE_TYPE_DRIVER    (1 << 6)
#define DRM_MODE_FLAG_PHSYNC    (1 << 0)
#define DRM_MODE_FLAG_NHSYNC    (1 << 1)
#define DRM_MODE_FLAG_PVSYNC    (1 << 2)
#define DRM_MODE_FLAG_NVSYNC    (1 << 3)

#define DRM_MODE_ENCODER_TMDS    2
#define DRM_MODE_ENCODER_VIRTUAL 5
#define DRM_MODE_CONNECTOR_eDP     14
#define DRM_MODE_CONNECTOR_VIRTUAL 15
#define DRM_MODE_CONNECTED       1
#define DRM_MODE_SUBPIXEL_UNKNOWN 1

#define DRM_MODE_PROP_RANGE     (1 << 1)
#define DRM_MODE_PROP_IMMUTABLE (1 << 2)
#define DRM_MODE_PROP_ENUM      (1 << 3)
#define DRM_MODE_PROP_BLOB      (1 << 4)

#define DRM_MODE_OBJECT_CRTC      0xcccccccc
#define DRM_MODE_OBJECT_CONNECTOR 0xc0c0c0c0
#define DRM_MODE_OBJECT_ENCODER   0xe0e0e0e0
#define DRM_MODE_OBJECT_PROPERTY  0xb0b0b0b0
#define DRM_MODE_OBJECT_FB        0xfbfbfbfb
#define DRM_MODE_OBJECT_BLOB      0xbbbbbbbb
#define DRM_MODE_OBJECT_PLANE     0xeeeeeeee
#define DRM_MODE_OBJECT_ANY       0

#define DRM_MODE_FB_MODIFIERS (1 << 1)
#define DRM_FORMAT_MOD_LINEAR 0ULL
#define DRM_MODE_FB_DIRTY_MAX_CLIPS 256

#define DRM_MODE_CURSOR_BO   0x01
#define DRM_MODE_CURSOR_MOVE 0x02

#define DRM_MODE_PAGE_FLIP_EVENT 0x01
#define DRM_MODE_PAGE_FLIP_ASYNC 0x02
#define DRM_MODE_PAGE_FLIP_TARGET_ABSOLUTE 0x4
#define DRM_MODE_PAGE_FLIP_TARGET_RELATIVE 0x8

#define DRM_PLANE_TYPE_OVERLAY 0
#define DRM_PLANE_TYPE_PRIMARY 1
#define DRM_PLANE_TYPE_CURSOR  2

#define DRM_FOURCC(a, b, c, d) ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))
#define DRM_FORMAT_XRGB8888 DRM_FOURCC('X', 'R', '2', '4')
#define DRM_FORMAT_ARGB8888 DRM_FOURCC('A', 'R', '2', '4')
#define DRM_FORMAT_XBGR8888 DRM_FOURCC('X', 'B', '2', '4')
#define DRM_FORMAT_ABGR8888 DRM_FOURCC('A', 'B', '2', '4')
#define DRM_FORMAT_RGB565   DRM_FOURCC('R', 'G', '1', '6')

struct drm_mode_modeinfo {
	uint32_t clock;
	uint16_t hdisplay, hsync_start, hsync_end, htotal, hskew;
	uint16_t vdisplay, vsync_start, vsync_end, vtotal, vscan;
	uint32_t vrefresh;
	uint32_t flags;
	uint32_t type;
	char     name[DRM_DISPLAY_MODE_LEN];
};
struct drm_mode_card_res {
	uint64_t fb_id_ptr, crtc_id_ptr, connector_id_ptr, encoder_id_ptr;
	uint32_t count_fbs, count_crtcs, count_connectors, count_encoders;
	uint32_t min_width, max_width, min_height, max_height;
};
struct drm_mode_crtc {
	uint64_t set_connectors_ptr;
	uint32_t count_connectors;
	uint32_t crtc_id, fb_id;
	uint32_t x, y;
	uint32_t gamma_size;
	uint32_t mode_valid;
	struct drm_mode_modeinfo mode;
};
struct drm_mode_set_plane {
	uint32_t plane_id, crtc_id, fb_id, flags;
	int32_t  crtc_x, crtc_y;
	uint32_t crtc_w, crtc_h;
	uint32_t src_x, src_y, src_h, src_w;   /* 16.16 */
};
struct drm_mode_get_plane {
	uint32_t plane_id, crtc_id, fb_id, possible_crtcs, gamma_size, count_format_types;
	uint64_t format_type_ptr;
};
struct drm_mode_get_plane_res { uint64_t plane_id_ptr; uint32_t count_planes; };
struct drm_mode_get_encoder { uint32_t encoder_id, encoder_type, crtc_id, possible_crtcs, possible_clones; };
struct drm_mode_get_connector {
	uint64_t encoders_ptr, modes_ptr, props_ptr, prop_values_ptr;
	uint32_t count_modes, count_props, count_encoders;
	uint32_t encoder_id, connector_id, connector_type, connector_type_id;
	uint32_t connection, mm_width, mm_height, subpixel, pad;
};
struct drm_mode_property_enum { uint64_t value; char name[DRM_PROP_NAME_LEN]; };
struct drm_mode_get_property {
	uint64_t values_ptr, enum_blob_ptr;
	uint32_t prop_id, flags;
	char     name[DRM_PROP_NAME_LEN];
	uint32_t count_values, count_enum_blobs;
};
struct drm_mode_connector_set_property { uint64_t value; uint32_t prop_id, connector_id; };
struct drm_mode_obj_get_properties { uint64_t props_ptr, prop_values_ptr; uint32_t count_props, obj_id, obj_type; };
struct drm_mode_obj_set_property { uint64_t value; uint32_t prop_id, obj_id, obj_type; };
struct drm_mode_get_blob { uint32_t blob_id, length; uint64_t data; };
struct drm_mode_fb_cmd { uint32_t fb_id, width, height, pitch, bpp, depth, handle; };
struct drm_mode_fb_cmd2 {
	uint32_t fb_id, width, height, pixel_format, flags;
	uint32_t handles[4], pitches[4], offsets[4];
	uint64_t modifier[4];
};
struct drm_mode_fb_dirty_cmd { uint32_t fb_id, flags, color, num_clips; uint64_t clips_ptr; };
struct drm_clip_rect { uint16_t x1, y1, x2, y2; };
struct drm_mode_cursor { uint32_t flags, crtc_id; int32_t x, y; uint32_t width, height, handle; };
struct drm_mode_cursor2 { uint32_t flags, crtc_id; int32_t x, y; uint32_t width, height, handle; int32_t hot_x, hot_y; };
struct drm_mode_crtc_lut { uint32_t crtc_id, gamma_size; uint64_t red, green, blue; };
struct drm_mode_crtc_page_flip { uint32_t crtc_id, fb_id, flags, reserved; uint64_t user_data; };
struct drm_mode_create_dumb { uint32_t height, width, bpp, flags, handle, pitch; uint64_t size; };
struct drm_mode_map_dumb { uint32_t handle, pad; uint64_t offset; };
struct drm_mode_destroy_dumb { uint32_t handle; };
struct drm_mode_create_blob { uint64_t data; uint32_t length, blob_id; };
struct drm_mode_destroy_blob { uint32_t blob_id; };
struct drm_mode_closefb { uint32_t fb_id, pad; };

/* dma-buf fds ('b') */
#define DMA_BUF_IOCTL_SYNC             0x40086200u  /* _IOW('b', 0, struct dma_buf_sync) */
#define DMA_BUF_SET_NAME_A             0x40046201u
#define DMA_BUF_SET_NAME_B             0x40086201u
#define DMA_BUF_IOCTL_EXPORT_SYNC_FILE 0xc0086202u  /* { u32 flags; s32 fd; } */
#define DMA_BUF_IOCTL_IMPORT_SYNC_FILE 0x40086203u
/* sync_file ('>') */
#define SYNC_IOC_MERGE                 0xc0303e03u
#define SYNC_IOC_FILE_INFO             0xc0383e04u

#endif
