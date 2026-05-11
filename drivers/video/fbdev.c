#include <kernel/vfs.h>
#include <kernel/fb.h>
#include <kernel/lib/string.h>
#include <kernel/mm/mmu.h>
#include <kernel/fs/devfs.h>

extern fb_info_t global_fb;
extern uint64_t  global_fb_phys;

#define FBIOGET_VSCREENINFO 0x4600
#define FBIOGET_FSCREENINFO 0x4602

struct fb_fix_screeninfo {
	char id[16];
	uint64_t smem_start;
	uint32_t smem_len;
	uint32_t type;
	uint32_t type_aux;
	uint32_t visual;
	uint16_t xpanstep;
	uint16_t ypanstep;
	uint16_t ywrapstep;
	uint32_t line_length;
	uint64_t mmio_start;
	uint32_t mmio_len;
	uint32_t accel;
	uint16_t capabilities;
	uint16_t reserved[2];
};

struct fb_var_screeninfo {
	uint32_t xres;
	uint32_t yres;
	uint32_t xres_virtual;
	uint32_t yres_virtual;
	uint32_t xoffset;
	uint32_t yoffset;
	uint32_t bits_per_pixel;
	uint32_t grayscale;
	struct { uint32_t offset, length, msb_right; } red, green, blue, transp;
	uint32_t nonstd;
	uint32_t activate;
	uint32_t height;
	uint32_t width;
	uint32_t accel_flags;
	uint32_t pixclock;
	uint32_t left_margin, right_margin, upper_margin, lower_margin;
	uint32_t hsync_len, vsync_len;
	uint32_t sync;
	uint32_t vmode;
	uint32_t reserved[6];
};

static uint64_t fbdev_mmap(vfs_file_t *file, off_t offset)
{
	(void)file;
	if (!global_fb_phys) return 0;
	if (offset >= global_fb.fb_size) return 0;
	return global_fb_phys + offset;
}

extern int copy_to_user(void *dst, const void *src, size_t size);

static int fbdev_ioctl(vfs_file_t *file, uint32_t cmd, uint64_t arg)
{
	(void)file;
	if (cmd == FBIOGET_VSCREENINFO) {
		struct fb_var_screeninfo v = {0};
		v.xres = global_fb.width;
		v.yres = global_fb.height;
		v.xres_virtual = global_fb.width;
		v.yres_virtual = global_fb.virt_height ? global_fb.virt_height : global_fb.height;
		v.bits_per_pixel = global_fb.depth;
		v.red.length = 8; v.red.offset = 16;
		v.green.length = 8; v.green.offset = 8;
		v.blue.length = 8; v.blue.offset = 0;
		v.transp.length = 8; v.transp.offset = 24;
		if (copy_to_user((void *)(uintptr_t)arg, &v, sizeof(v)) != 0) return -14;
		return 0;
	}
	if (cmd == FBIOGET_FSCREENINFO) {
		struct fb_fix_screeninfo f = {0};
		memcpy(f.id, "ranixfb", 8);
		f.smem_start = global_fb_phys;
		f.smem_len = global_fb.fb_size;
		f.line_length = global_fb.pitch;
		if (copy_to_user((void *)(uintptr_t)arg, &f, sizeof(f)) != 0) return -14;
		return 0;
	}
	return -25;
}

static vfs_fops_t fbdev_fops = {
	.mmap  = fbdev_mmap,
	.ioctl = fbdev_ioctl,
};

void fbdev_init(void)
{
	if (global_fb_phys) {
		devfs_register_device("fb0", S_IFCHR | 0666, 0x1d00, &fbdev_fops);
	}
}
