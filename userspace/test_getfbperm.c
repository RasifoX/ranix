/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* GETFB hands out a handle to a framebuffer's pixels only to the DRM master or to
 * CAP_SYS_ADMIN, as on Linux. The master puts a pattern in a dumb buffer and adds
 * it as a framebuffer; a second opener without CAP_SYS_ADMIN gets the size but
 * handle 0; one with CAP_SYS_ADMIN gets a handle and reads the same pattern back
 * (what `fjord screenshot` does as root). OK when the machine has no DRM device.
 * First failure or OK. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>

struct drm_mode_fb_cmd { uint32_t fb_id, width, height, pitch, bpp, depth, handle; };
struct drm_mode_create_dumb { uint32_t height, width, bpp, flags, handle, pitch; uint64_t size; };
struct drm_mode_map_dumb { uint32_t handle, pad; uint64_t offset; };
#define D(n, t) _IOWR('d', n, t)
#define IOC_GETFB       D(0xAD, struct drm_mode_fb_cmd)
#define IOC_ADDFB       D(0xAE, struct drm_mode_fb_cmd)
#define IOC_CREATE_DUMB D(0xB2, struct drm_mode_create_dumb)
#define IOC_MAP_DUMB    D(0xB3, struct drm_mode_map_dumb)
#define CAP_SYS_ADMIN 21
#define W 64
#define H 32

typedef struct { unsigned version; int pid; } cap_hdr;
typedef struct { unsigned effective, permitted, inheritable; } cap_data;

static uint32_t px(int x, int y) { return 0xFF000000u | (uint32_t)(x * 4) << 16 | (uint32_t)(y * 8) << 8 | 0x5A; }

static int drop_sys_admin(void)
{
	cap_hdr h = { 0x20080522, 0 };
	cap_data d[2];
	if (syscall(SYS_capget, &h, d) != 0) return -1;
	d[0].effective &= ~(1u << CAP_SYS_ADMIN);
	d[0].permitted &= ~(1u << CAP_SYS_ADMIN);
	return (int)syscall(SYS_capset, &h, d);
}

/* child: 0 = as expected; 1.. = what went wrong */
static int reader(uint32_t fb_id, int admin)
{
	if (!admin && drop_sys_admin()) return 1;
	int fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
	if (fd < 0) return 2;
	struct drm_mode_fb_cmd g = { .fb_id = fb_id };
	if (ioctl(fd, IOC_GETFB, &g) != 0) return 3;
	if (g.width != W || g.height != H) return 4;
	if (!admin) return g.handle == 0 ? 0 : 5;
	if (!g.handle) return 6;
	struct drm_mode_map_dumb m = { .handle = g.handle };
	if (ioctl(fd, IOC_MAP_DUMB, &m) != 0) return 7;
	uint8_t *p = mmap(NULL, (size_t)g.pitch * H, PROT_READ, MAP_SHARED, fd, (off_t)m.offset);
	if (p == MAP_FAILED) return 8;
	for (int y = 0; y < H; y++)
		for (int x = 0; x < W; x++)
			if (((uint32_t *)(p + (size_t)y * g.pitch))[x] != px(x, y)) return 9;
	return 0;
}

static int run(uint32_t fb_id, int admin)
{
	pid_t c = fork();
	if (c == 0) _exit(reader(fb_id, admin));
	int st = 0;
	waitpid(c, &st, 0);
	return WIFEXITED(st) ? WEXITSTATUS(st) : 100;
}

int main(void)
{
	int fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
	if (fd < 0) { puts("OK"); return 0; }     /* no DRM here */
	struct drm_mode_create_dumb c = { .height = H, .width = W, .bpp = 32 };
	if (ioctl(fd, IOC_CREATE_DUMB, &c) != 0) { printf("FAIL create_dumb errno %d\n", errno); return 1; }
	struct drm_mode_map_dumb m = { .handle = c.handle };
	if (ioctl(fd, IOC_MAP_DUMB, &m) != 0) { puts("FAIL map_dumb"); return 1; }
	uint8_t *p = mmap(NULL, c.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)m.offset);
	if (p == MAP_FAILED) { puts("FAIL mmap"); return 1; }
	for (int y = 0; y < H; y++)
		for (int x = 0; x < W; x++) ((uint32_t *)(p + (size_t)y * c.pitch))[x] = px(x, y);
	struct drm_mode_fb_cmd a = { .width = W, .height = H, .pitch = c.pitch, .bpp = 32, .depth = 24, .handle = c.handle };
	if (ioctl(fd, IOC_ADDFB, &a) != 0) { printf("FAIL addfb errno %d\n", errno); return 1; }
	struct drm_mode_fb_cmd g = { .fb_id = a.fb_id };
	if (ioctl(fd, IOC_GETFB, &g) != 0 || !g.handle) { puts("FAIL the master got no handle"); return 1; }
	int r = run(a.fb_id, 0);
	if (r) { printf("FAIL without CAP_SYS_ADMIN: %d (5 = got a handle)\n", r); return 1; }
	r = run(a.fb_id, 1);
	if (r) { printf("FAIL with CAP_SYS_ADMIN: %d\n", r); return 1; }
	puts("OK");
	return 0;
}
