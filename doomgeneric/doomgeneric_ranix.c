#include "doomkeys.h"
#include "doomgeneric.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <time.h>

/* Linux-compat fbdev ioctls (same values as fbdev.c) */
#define FBIOGET_VSCREENINFO 0x4600
#define FBIOGET_FSCREENINFO 0x4602

struct fb_fix_screeninfo {
	char     id[16];
	uint64_t smem_start;
	uint32_t smem_len;
	uint32_t type;
	uint32_t type_aux;
	uint32_t visual;
	uint16_t xpanstep, ypanstep, ywrapstep;
	uint32_t line_length;
	uint64_t mmio_start;
	uint32_t mmio_len;
	uint32_t accel;
	uint16_t capabilities;
	uint16_t reserved[2];
};

struct fb_var_screeninfo {
	uint32_t xres, yres;
	uint32_t xres_virtual, yres_virtual;
	uint32_t xoffset, yoffset;
	uint32_t bits_per_pixel;
	uint32_t grayscale;
	struct { uint32_t offset, length, msb_right; } red, green, blue, transp;
	uint32_t nonstd, activate, height, width, accel_flags;
	uint32_t pixclock, left_margin, right_margin, upper_margin, lower_margin;
	uint32_t hsync_len, vsync_len, sync, vmode;
	uint32_t reserved[6];
};

/* input_event from /dev/input/event0 */
#define EV_KEY 0x01

struct input_event {
	struct { int64_t tv_sec; int64_t tv_usec; } time;
	uint16_t type;
	uint16_t code;
	int32_t  value;
};

/* PS/2 scancode set 1 → doom key mapping (same scancodes as ps2.c) */
static unsigned char sc_to_doom(uint16_t sc)
{
	switch (sc) {
	case 0x01: return KEY_ESCAPE;
	case 0x1C: return KEY_ENTER;
	case 0x4B: return KEY_LEFTARROW;
	case 0x4D: return KEY_RIGHTARROW;
	case 0x48: return KEY_UPARROW;
	case 0x50: return KEY_DOWNARROW;
	case 0x1D: return KEY_FIRE;
	case 0x39: return KEY_USE;
	case 0x2A: case 0x36: return KEY_RSHIFT;
	case 0x38: return KEY_LALT;
	case 0x15: return 'y';
	case 0x2C: return 'z';
	default:   return 0;
	}
}

#define KEYQUEUE_SIZE 32
static unsigned short s_keyq[KEYQUEUE_SIZE];
static int s_kq_w = 0, s_kq_r = 0;

static int s_fb_fd = -1;
static uint32_t *s_fb = NULL;
static uint32_t s_sw = 0, s_sh = 0, s_pitch = 0;

static int s_ev_fd = -1;

void DG_Init(void)
{
	struct fb_var_screeninfo var;
	struct fb_fix_screeninfo fix;

	s_fb_fd = open("/dev/fb0", O_RDWR);
	if (s_fb_fd < 0) { fprintf(stderr, "doom: /dev/fb0 open failed\n"); exit(1); }

	if (ioctl(s_fb_fd, FBIOGET_VSCREENINFO, &var) < 0) {
		fprintf(stderr, "doom: FBIOGET_VSCREENINFO failed\n"); exit(1);
	}
	if (ioctl(s_fb_fd, FBIOGET_FSCREENINFO, &fix) < 0) {
		fprintf(stderr, "doom: FBIOGET_FSCREENINFO failed\n"); exit(1);
	}

	s_sw    = var.xres;
	s_sh    = var.yres;
	s_pitch = fix.line_length / 4;

	s_fb = mmap(NULL, fix.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, s_fb_fd, 0);
	if (s_fb == MAP_FAILED) { fprintf(stderr, "doom: fb mmap failed\n"); exit(1); }

	s_ev_fd = open("/dev/input/event0", O_RDONLY | O_NONBLOCK);

	printf("doom: fb %dx%d, pitch=%d\n", s_sw, s_sh, s_pitch);
}

void DG_DrawFrame(void)
{
	uint32_t sx = s_sw / DOOMGENERIC_RESX;
	uint32_t sy = s_sh / DOOMGENERIC_RESY;
	uint32_t scale = sx < sy ? sx : sy;
	if (scale < 1) scale = 1;

	uint32_t dw = DOOMGENERIC_RESX * scale;
	uint32_t dh = DOOMGENERIC_RESY * scale;
	uint32_t ox = (s_sw - dw) / 2;
	uint32_t oy = (s_sh - dh) / 2;

	for (uint32_t y = 0; y < (uint32_t)DOOMGENERIC_RESY; y++) {
		const uint32_t *src = DG_ScreenBuffer + y * DOOMGENERIC_RESX;
		for (uint32_t ys = 0; ys < scale; ys++) {
			uint32_t *dst = s_fb + (oy + y * scale + ys) * s_pitch + ox;
			for (uint32_t x = 0; x < (uint32_t)DOOMGENERIC_RESX; x++) {
				uint32_t px = src[x];
				for (uint32_t xs = 0; xs < scale; xs++)
					dst[x * scale + xs] = px;
			}
		}
	}

	if (s_ev_fd < 0) return;
	struct input_event ev;
	while (read(s_ev_fd, &ev, sizeof(ev)) == sizeof(ev)) {
		if (ev.type != EV_KEY) continue;
		unsigned char dk = sc_to_doom(ev.code);
		if (!dk) continue;
		unsigned short kd = ((ev.value ? 1 : 0) << 8) | dk;
		s_keyq[s_kq_w] = kd;
		s_kq_w = (s_kq_w + 1) % KEYQUEUE_SIZE;
	}
}

void DG_SleepMs(uint32_t ms)
{
	struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000 };
	nanosleep(&ts, NULL);
}

uint32_t DG_GetTicksMs(void)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (uint32_t)(tv.tv_sec * 1000 + tv.tv_usec / 1000);
}

int DG_GetKey(int *pressed, unsigned char *doomKey)
{
	if (s_kq_r == s_kq_w) return 0;
	unsigned short kd = s_keyq[s_kq_r];
	s_kq_r = (s_kq_r + 1) % KEYQUEUE_SIZE;
	*pressed = kd >> 8;
	*doomKey = kd & 0xFF;
	return 1;
}

void DG_SetWindowTitle(const char *title) { (void)title; }

int main(int argc, char **argv)
{
	doomgeneric_Create(argc, argv);
	while (1)
		doomgeneric_Tick();
	return 0;
}
