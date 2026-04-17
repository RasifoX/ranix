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
	/* platform scroll hook: pan virtual framebuffer; NULL = software fallback */
	int (*set_virt_offset)(u32 x, u32 y);
} fb_info_t;

#endif
