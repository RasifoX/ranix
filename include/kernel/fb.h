/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
#ifndef FB_H
#define FB_H

#include <uapi/types.h>

typedef struct {
	u32   width;
	u32   height;
	u32   virt_height;
	u32   pitch;
	u32   depth;
	void *fb;
	u32   fb_size;
	/* RGB channel bit positions (from framebuffer info) */
	u8    red_shift;
	u8    green_shift;
	u8    blue_shift;
	/* platform scroll hook: pan virtual framebuffer; NULL = software fallback */
	int (*set_virt_offset)(u32 x, u32 y);
} fb_info_t;

extern fb_info_t global_fb;                      /* the boot framebuffer (main.c) */
extern uint64_t  global_fb_phys;

/* drivers/video/backlight.c */
int  backlight_get(void);
int  backlight_set(int pct);
void backlight_unblank(void);
int  igd_scanline(uint32_t *total);

#endif
