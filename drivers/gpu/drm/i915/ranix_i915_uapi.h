/* SPDX-License-Identifier: MIT */
/*
 * The i915 user ABI Ranix implements: ioctl numbers, structs and constants as in
 * Linux include/uapi/drm/i915_drm.h (they are the ABI; layouts checked against the
 * Linux header with static asserts). Ranix's own transcription.
 */
#ifndef RANIX_I915_UAPI_H
#define RANIX_I915_UAPI_H
#include <uapi/types.h>

#define DRM_I915_GETPARAM          0x06
#define DRM_I915_GEM_BUSY          0x17
#define DRM_I915_GEM_THROTTLE      0x18
#define DRM_I915_GEM_CREATE        0x1b
#define DRM_I915_GEM_PREAD         0x1c
#define DRM_I915_GEM_PWRITE        0x1d
#define DRM_I915_GEM_MMAP          0x1e
#define DRM_I915_GEM_SET_DOMAIN    0x1f
#define DRM_I915_GEM_SW_FINISH     0x20
#define DRM_I915_GEM_SET_TILING    0x21
#define DRM_I915_GEM_GET_TILING    0x22
#define DRM_I915_GEM_GET_APERTURE  0x23
#define DRM_I915_GEM_MMAP_OFFSET   0x24
#define DRM_I915_GEM_MADVISE       0x26
#define DRM_I915_GEM_EXECBUFFER2   0x29
#define DRM_I915_GEM_WAIT          0x2c
#define DRM_I915_GEM_CONTEXT_CREATE 0x2d
#define DRM_I915_GEM_CONTEXT_DESTROY 0x2e
#define DRM_I915_GEM_SET_CACHING   0x2f
#define DRM_I915_GEM_GET_CACHING   0x30
#define DRM_I915_REG_READ          0x31
#define DRM_I915_GET_RESET_STATS   0x32
#define DRM_I915_GEM_USERPTR       0x33
#define DRM_I915_GEM_CONTEXT_GETPARAM 0x34
#define DRM_I915_GEM_CONTEXT_SETPARAM 0x35
#define DRM_I915_QUERY             0x39
#define DRM_I915_GEM_VM_CREATE     0x3a
#define DRM_I915_GEM_VM_DESTROY    0x3b
#define DRM_I915_GEM_CREATE_EXT    0x3c
#define DRM_I915_NIOCTLS           0x3d

#define I915_PARAM_CHIPSET_ID            4
#define I915_PARAM_REVISION              32
#define I915_PARAM_SUBSLICE_TOTAL        33
#define I915_PARAM_EU_TOTAL              34
#define I915_PARAM_HAS_GPU_RESET         35
#define I915_PARAM_HAS_SCHEDULER         41
#define I915_PARAM_SLICE_MASK            46
#define I915_PARAM_SUBSLICE_MASK         47
#define I915_PARAM_HAS_CONTEXT_ISOLATION 50
#define I915_PARAM_CS_TIMESTAMP_FREQUENCY 51

struct drm_i915_getparam { int32_t param; uint32_t pad_; uint64_t value; };
struct drm_i915_gem_create { uint64_t size; uint32_t handle, pad; };
struct i915_user_extension { uint64_t next_extension; uint32_t name, flags; uint32_t rsvd[4]; };
struct drm_i915_gem_create_ext { uint64_t size; uint32_t handle, flags; uint64_t extensions; };
#define I915_GEM_CREATE_EXT_MEMORY_REGIONS 0
struct drm_i915_gem_memory_class_instance { uint16_t memory_class, memory_instance; };
struct drm_i915_gem_create_ext_memory_regions { struct i915_user_extension base; uint32_t pad, num_regions; uint64_t regions; };
struct drm_i915_gem_pread { uint32_t handle, pad; uint64_t offset, size, data_ptr; };
struct drm_i915_gem_pwrite { uint32_t handle, pad; uint64_t offset, size, data_ptr; };
struct drm_i915_gem_mmap_offset { uint32_t handle, pad; uint64_t offset, flags, extensions; };
#define I915_MMAP_OFFSET_FIXED 4
struct drm_i915_gem_set_domain { uint32_t handle, read_domains, write_domain; };
struct drm_i915_gem_sw_finish { uint32_t handle; };
struct drm_i915_gem_busy { uint32_t handle, busy; };
struct drm_i915_gem_wait { uint32_t bo_handle, flags; int64_t timeout_ns; };
struct drm_i915_gem_get_aperture { uint64_t aper_size, aper_available_size; };
struct drm_i915_gem_set_tiling { uint32_t handle, tiling_mode, stride, swizzle_mode; };
struct drm_i915_gem_get_tiling { uint32_t handle, tiling_mode, swizzle_mode, phys_swizzle_mode; };
struct drm_i915_gem_madvise { uint32_t handle, madv, retained; };
struct drm_i915_gem_caching { uint32_t handle, caching; };
struct drm_i915_reg_read { uint64_t offset, val; };
struct drm_i915_reset_stats { uint32_t ctx_id, flags, reset_count, batch_active, batch_pending, pad; };

struct drm_i915_gem_context_create_ext { uint32_t ctx_id, flags; uint64_t extensions; };
#define I915_CONTEXT_CREATE_FLAGS_USE_EXTENSIONS  (1u << 0)
#define I915_CONTEXT_CREATE_FLAGS_SINGLE_TIMELINE (1u << 1)
#define I915_CONTEXT_CREATE_EXT_SETPARAM 0
struct drm_i915_gem_context_destroy { uint32_t ctx_id, pad; };
struct drm_i915_gem_context_param { uint32_t ctx_id, size; uint64_t param, value; };
struct drm_i915_gem_context_create_ext_setparam { struct i915_user_extension base; struct drm_i915_gem_context_param param; };
#define I915_CONTEXT_PARAM_BAN_PERIOD   0x1
#define I915_CONTEXT_PARAM_NO_ZEROMAP   0x2
#define I915_CONTEXT_PARAM_GTT_SIZE     0x3
#define I915_CONTEXT_PARAM_NO_ERROR_CAPTURE 0x4
#define I915_CONTEXT_PARAM_BANNABLE     0x5
#define I915_CONTEXT_PARAM_PRIORITY     0x6
#define I915_CONTEXT_PARAM_SSEU         0x7
#define I915_CONTEXT_PARAM_RECOVERABLE  0x8
#define I915_CONTEXT_PARAM_VM           0x9
#define I915_CONTEXT_PARAM_ENGINES      0xa
#define I915_CONTEXT_PARAM_PERSISTENCE  0xb
#define I915_CONTEXT_PARAM_RINGSIZE     0xc
struct i915_engine_class_instance { uint16_t engine_class, engine_instance; };
#define I915_ENGINE_CLASS_RENDER 0

struct drm_i915_gem_execbuffer2 {
	uint64_t buffers_ptr;
	uint32_t buffer_count, batch_start_offset, batch_len, DR1, DR4, num_cliprects;
	uint64_t cliprects_ptr, flags, rsvd1, rsvd2;
};
#define I915_EXEC_RING_MASK        0x3f
#define I915_EXEC_SECURE           (1 << 9)
#define I915_EXEC_IS_PINNED        (1 << 10)
#define I915_EXEC_NO_RELOC         (1 << 11)
#define I915_EXEC_HANDLE_LUT       (1 << 12)
#define I915_EXEC_FENCE_IN         (1 << 16)
#define I915_EXEC_FENCE_OUT        (1 << 17)
#define I915_EXEC_BATCH_FIRST      (1 << 18)
#define I915_EXEC_FENCE_ARRAY      (1 << 19)
#define I915_EXEC_FENCE_SUBMIT     (1 << 20)
#define I915_EXEC_USE_EXTENSIONS   (1 << 21)
#define I915_EXEC_KNOWN_FLAGS      ((1ULL << 22) - 1)
struct drm_i915_gem_exec_object2 {
	uint32_t handle, relocation_count;
	uint64_t relocs_ptr, alignment, offset, flags, rsvd1, rsvd2;
};
#define EXEC_OBJECT_WRITE               (1 << 2)
#define EXEC_OBJECT_SUPPORTS_48B_ADDRESS (1 << 3)
#define EXEC_OBJECT_PINNED              (1 << 4)
#define EXEC_OBJECT_PAD_TO_SIZE         (1 << 5)
#define EXEC_OBJECT_KNOWN_FLAGS         ((1 << 8) - 1)
struct drm_i915_gem_relocation_entry {
	uint32_t target_handle, delta;
	uint64_t offset, presumed_offset;
	uint32_t read_domains, write_domain;
};
struct drm_i915_gem_exec_fence { uint32_t handle, flags; };
#define I915_EXEC_FENCE_WAIT   (1 << 0)
#define I915_EXEC_FENCE_SIGNAL (1 << 1)

struct drm_i915_query { uint32_t num_items, flags; uint64_t items_ptr; };
struct drm_i915_query_item { uint64_t query_id; int32_t length; uint32_t flags; uint64_t data_ptr; };
#define DRM_I915_QUERY_TOPOLOGY_INFO  1
#define DRM_I915_QUERY_ENGINE_INFO    2
#define DRM_I915_QUERY_MEMORY_REGIONS 4
struct drm_i915_query_topology_info {
	uint16_t flags, max_slices, max_subslices, max_eus_per_subslice;
	uint16_t subslice_offset, subslice_stride, eu_offset, eu_stride;
	uint8_t data[];
};
struct drm_i915_engine_info {
	struct i915_engine_class_instance engine;
	uint32_t rsvd0;
	uint64_t flags, capabilities;
	uint16_t logical_instance, rsvd1[3];
	uint64_t rsvd2[3];
};
struct drm_i915_query_engine_info { uint32_t num_engines, rsvd[3]; struct drm_i915_engine_info engines[]; };
struct drm_i915_memory_region_info {
	struct drm_i915_gem_memory_class_instance region;
	uint32_t rsvd0;
	uint64_t probed_size, unallocated_size;
	uint64_t rsvd1[8];
};
struct drm_i915_query_memory_regions { uint32_t num_regions, rsvd[3]; struct drm_i915_memory_region_info regions[]; };

#endif
