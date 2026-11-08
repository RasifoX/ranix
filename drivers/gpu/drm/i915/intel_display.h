/* SPDX-License-Identifier: MIT */
/* What the Intel display back end (intel_display.c) offers the render engine and
 * the PPGTT: the register BAR, kernel objects in the GGTT window, and the checks
 * that keep every GPU page-table entry on RAM or on the scratch page. */
#ifndef INTEL_DISPLAY_H
#define INTEL_DISPLAY_H
#include "../drm_priv.h"

volatile uint8_t *intel_mmio(void);          /* NULL when no Intel display was found */
int      intel_ggtt_bind(drm_gem_t *o);      /* into our GGTT window; o->ggtt set */
uint64_t intel_scratch_page(void);           /* zeroed page behind unused entries; 0 if none */
int      intel_phys_ok(uint64_t pa);         /* page aligned and managed by the page allocator */
int      intel_ggtt_audit(char *o, int max); /* read-only report of every GGTT entry */
const drm_driver_t *ranix_i915_init(void);   /* GEM/execbuffer ioctls (ranix_gem.c) */

#endif
